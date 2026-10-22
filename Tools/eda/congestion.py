"""Estimates routing congestion on a placed board before routing, and draws it as a heat map.

Run it with KiCad's own Python after place.py:

  "C:/Program Files/KiCad/9.0/bin/python.exe" Tools/eda/congestion.py "Build/KiCad/Register File"

Given a project directory it checks every placement in Candidates/ (or the project's board if there are none);
it also takes .kicad_pcb files directly. Each board gets <board>-congestion.svg beside it.

The estimate is RUDY (Rectangular Uniform wire DensitY): each signal net needs about its bounding box's
half-perimeter of track, spread evenly over that box. Summed over all nets, that is the track each tile of the
board is asked to carry, in mm of track per mm2. The board can carry signal layers / (track + clearance) of it,
the signal layers being the copper ones with no plane (2, or 3 after place.py --signal-layers 3), and the track
and clearance the Default net class's: one track per routing pitch on each signal layer, less what the pads block.
Each pad, grown by a clearance and half a track, takes its share of its tiles' supply: a through-hole pad on every
signal layer, a surface pad (SOIC, 0805) on its own layer only, so 1 / signal layers of it. A surface pad on a
plane's net (a chip's VCC or GND pin) also has a via down to the plane, which blocks every layer. Utilisation is
demand over that supply. The nets on planes (GND, and VCC unless it's routed) are left out of the demand, since they
need no tracks, but their pads still block.

Utilisation is optimistic on purpose: tracks must turn, change layers and dodge pads, so routers usually start
failing well before 1.0. Read about 0.5 as busy and 0.7 and up as likely trouble. The overflow score adds up
how far tiles go past 0.5, so a lower score is an easier board; compare candidates by it.
"""
import argparse
import glob
import math
import os
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kicad_env import NET_CLASSES  # noqa: E402

TRACK, CLEARANCE, VIA = NET_CLASSES[0][1:4]  # mm: the Default net class's, which most nets are in
BUSY = 0.5                    # utilisation where trouble starts
MOST_BLOCKED = 0.9            # pads never take more of a tile than this: a track can still squeeze through
PX = 4                        # SVG pixels per mm


def load(path):
    """(board size, {net: [pad points]}, [(reference, x0, y0, x1, y1)], [pad boxes (x0, y0, x1, y1, through)],
    signal layers), in mm from the board's corner; a pad is through when it blocks every layer."""
    board = pcbnew.LoadBoard(path)
    box = board.GetBoardEdgesBoundingBox()
    ox, oy = pcbnew.ToMM(box.GetX()), pcbnew.ToMM(box.GetY())
    size = (pcbnew.ToMM(box.GetWidth()), pcbnew.ToMM(box.GetHeight()))
    inner = [z for z in board.Zones() if not z.GetIsRuleArea() and z.GetNetname()
             and z.GetLayer() not in (pcbnew.F_Cu, pcbnew.B_Cu)]
    planes = {z.GetNetname().lstrip('/') for z in inner}  # nets the planes carry, which need no tracks
    layers = board.GetCopperLayerCount() - len({z.GetLayer() for z in inner})
    nets, bodies, pads = {}, [], []
    for footprint in board.GetFootprints():
        body = footprint.GetBoundingBox(False)
        bodies.append((footprint.GetReference(), pcbnew.ToMM(body.GetLeft()) - ox, pcbnew.ToMM(body.GetTop()) - oy,
                       pcbnew.ToMM(body.GetRight()) - ox, pcbnew.ToMM(body.GetBottom()) - oy))
        for pad in footprint.Pads():
            area, at, name = pad.GetBoundingBox(), pad.GetPosition(), pad.GetNetname()
            surface = pad.GetAttribute() == pcbnew.PAD_ATTRIB_SMD
            pads.append((pcbnew.ToMM(area.GetLeft()) - ox, pcbnew.ToMM(area.GetTop()) - oy,
                         pcbnew.ToMM(area.GetRight()) - ox, pcbnew.ToMM(area.GetBottom()) - oy, not surface))
            if not name or name.lstrip('/') in planes:
                if name and surface:
                    pads.append(via_box(pcbnew.ToMM(at.x) - ox, pcbnew.ToMM(at.y) - oy))
                continue
            nets.setdefault(name, []).append((pcbnew.ToMM(at.x) - ox, pcbnew.ToMM(at.y) - oy))
    return size, {name: points for name, points in nets.items() if len(points) > 1}, bodies, pads, layers


def via_box(x, y):
    """The pad box of a via at (x, y): a plane pin's, which sits beside its pad, so here on it."""
    return x - VIA / 2, y - VIA / 2, x + VIA / 2, y + VIA / 2, True


def blockage(size, pads, tile, signal_layers=2):
    """The share of each tile's supply its pads take, rows of columns, at most MOST_BLOCKED. A through pad blocks
    every signal layer, a surface one only its own."""
    cols, rows = max(1, math.ceil(size[0] / tile)), max(1, math.ceil(size[1] / tile))
    blocked = [[0.0] * cols for _ in range(rows)]
    grow = CLEARANCE + TRACK / 2
    for x0, y0, x1, y1, through in pads:
        x0, y0, x1, y1 = x0 - grow, y0 - grow, x1 + grow, y1 + grow
        share = 1 if through else 1 / signal_layers
        for row in range(max(0, int(y0 // tile)), min(rows - 1, int(y1 // tile)) + 1):
            dy = min(y1, (row + 1) * tile) - max(y0, row * tile)
            for col in range(max(0, int(x0 // tile)), min(cols - 1, int(x1 // tile)) + 1):
                dx = min(x1, (col + 1) * tile) - max(x0, col * tile)
                if dx > 0 and dy > 0:
                    blocked[row][col] += share * dx * dy / (tile * tile)
    return [[min(MOST_BLOCKED, value) for value in row] for row in blocked]


def rudy(size, nets, tile, pads=(), signal_layers=2):
    """Utilisation per tile, rows of columns; pads are boxes (x0, y0, x1, y1, through), as blockage takes them."""
    cols, rows = max(1, math.ceil(size[0] / tile)), max(1, math.ceil(size[1] / tile))
    demand = [[0.0] * cols for _ in range(rows)]
    for points in nets.values():
        xs, ys = [p[0] for p in points], [p[1] for p in points]
        x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
        if x1 - x0 < tile:  # a straight net still needs a lane: give its box a tile's width
            middle = (x0 + x1) / 2
            x0, x1 = middle - tile / 2, middle + tile / 2
        if y1 - y0 < tile:
            middle = (y0 + y1) / 2
            y0, y1 = middle - tile / 2, middle + tile / 2
        density = (x1 - x0 + y1 - y0) / ((x1 - x0) * (y1 - y0))  # mm of track per mm2 inside the box
        for row in range(max(0, int(y0 // tile)), min(rows - 1, int(y1 // tile)) + 1):
            dy = min(y1, (row + 1) * tile) - max(y0, row * tile)
            for col in range(max(0, int(x0 // tile)), min(cols - 1, int(x1 // tile)) + 1):
                dx = min(x1, (col + 1) * tile) - max(x0, col * tile)
                if dx > 0 and dy > 0:
                    demand[row][col] += density * dx * dy / (tile * tile)
    supply = signal_layers / (TRACK + CLEARANCE)
    blocked = blockage(size, pads, tile, signal_layers)
    return [[value / (supply * (1 - b)) for value, b in zip(row, blocks)] for row, blocks in zip(demand, blocked)]


def colour(u):
    """Green when quiet, through yellow at BUSY, to red at 1.0 and beyond."""
    t = max(0.0, min(1.0, u))
    if t < BUSY:
        r, g = int(255 * t / BUSY), 200
    else:
        r, g = 255, int(200 * (1 - (t - BUSY) / (1 - BUSY)))
    return '#%02x%02x40' % (r, g)


def svg(path, size, grid, tile, bodies, title):
    w, h = size[0] * PX, size[1] * PX
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
           'font-family="sans-serif">' % (w, h + 40, w, h + 40),
           '<rect width="%d" height="%d" fill="#fff"/>' % (w, h + 40)]
    for r, row in enumerate(grid):
        for c, u in enumerate(row):
            out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s" fill-opacity="0.85">'
                       '<title>%.2f</title></rect>' % (c * tile * PX, r * tile * PX, tile * PX, tile * PX,
                                                      colour(u), u))
    for reference, x0, y0, x1, y1 in bodies:
        out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="none" stroke="#222" '
                   'stroke-width="1"/>' % (x0 * PX, y0 * PX, (x1 - x0) * PX, (y1 - y0) * PX))
        out.append('<text x="%.1f" y="%.1f" font-size="9" text-anchor="middle" fill="#000">%s</text>'
                   % ((x0 + x1) / 2 * PX, (y0 + y1) / 2 * PX + 3, reference))
    out.append('<rect x="0" y="0" width="%d" height="%d" fill="none" stroke="#000" stroke-width="2"/>' % (w, h))
    out.append('<text x="8" y="%d" font-size="14">%s - green quiet, yellow %.1f, red 1.0+ '
               '(hover a tile for its value)</text>' % (h + 26, title, BUSY))
    out.append('</svg>')
    with open(path, 'w', encoding='utf-8') as file:
        file.write('\n'.join(out))


def report(path, tile):
    size, nets, bodies, pads, layers = load(path)
    grid = rudy(size, nets, tile, pads, layers)
    values = [u for row in grid for u in row]
    overflow = sum((u - BUSY) ** 2 for u in values if u > BUSY) * tile * tile
    print('%s: %d signal nets on %d layers, peak %.2f, mean %.2f, %.0f%% of tiles over %.1f, %.0f%% over 1.0; overflow %.0f'
          % (os.path.basename(path), len(nets), layers, max(values), sum(values) / len(values),
             100 * sum(u > BUSY for u in values) / len(values), BUSY,
             100 * sum(u > 1 for u in values) / len(values), overflow))
    hot = sorted(((u, r, c) for r, row in enumerate(grid) for c, u in enumerate(row)), reverse=True)
    shown = []
    for u, r, c in hot:
        if u <= BUSY or len(shown) == 5:
            break
        x, y = (c + 0.5) * tile, (r + 0.5) * tile
        if any(abs(x - sx) < 4 * tile and abs(y - sy) < 4 * tile for sx, sy in shown):
            continue  # one line per hot spot, not per tile in it
        shown.append((x, y))
        near = sorted(bodies, key=lambda b: math.hypot((b[1] + b[3]) / 2 - x, (b[2] + b[4]) / 2 - y))[:3]
        print('    hot spot %.2f at (%.0f, %.0f) mm, by %s' % (u, x, y, ', '.join(b[0] for b in near)))
    picture = os.path.splitext(path)[0] + '-congestion.svg'
    svg(picture, size, grid, tile, bodies, os.path.basename(path))
    return overflow, picture


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('boards', nargs='+', help='project directories or .kicad_pcb files')
    parser.add_argument('--tile', type=float, default=2.54, help='tile size in mm (default 2.54)')
    args = parser.parse_args()
    paths = []
    for item in args.boards:
        if os.path.isdir(item):
            name = os.path.basename(os.path.normpath(item))
            found = sorted(glob.glob(os.path.join(item, 'Candidates', '*.kicad_pcb')))
            paths += found or [os.path.join(item, name + '.kicad_pcb')]
        else:
            paths.append(item)
    results = [(report(path, args.tile), path) for path in paths]
    if len(results) > 1:
        (overflow, _), best = min(results)
        print('least congested: %s (overflow %.0f)' % (os.path.basename(best), overflow))


if __name__ == '__main__':
    main()