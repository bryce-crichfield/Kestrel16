"""Places a KiCad project's chips into a slot grid by simulated annealing and writes the board.

Run it with KiCad's own Python, which has the pcbnew module:

  "C:/Program Files/KiCad/9.0/bin/python.exe" Tools/eda/place.py "Build/KiCad/Register 16"

It reads the netlist kicad-cli exports from the schematic, freshly each run, so it sees exactly what the schematic
has. The board area is a grid of slots, one chip per slot, with spare slots so chips can move.
The anneal swaps or moves chips to shorten the weighted half-perimeter of every signal net; GND and VCC are skipped,
since they are the inner planes. Several independent anneals run. Then each 100nF cap goes beside a chip's VCC pin,
the headers go along the bottom edge with the bulk cap, and the board is written as <project>.kicad_pcb: 4 layers,
In1 a GND plane and In2 a VCC plane, ready for routing. The best few placements are also written as
Candidates/1.kicad_pcb, 2, ... for route.py, which routes each and keeps the best.
"""
import argparse
import math
import os
import random
import subprocess
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr  # noqa: E402
from kicad_env import POWER, fill_zones, find_kicad, kicad_cli, net_name  # noqa: E402
from sexpr import find, find_all  # noqa: E402

EDGE = 5.0       # mm from the board edge to the first slot or header
CAP_GAP = 2.5    # mm from a VCC pad to its cap
CHIP_ANGLE = 90  # locked rotation: every DIP lies on its side, pin 1 bottom left


def mm(value):
    return pcbnew.FromMM(value)


# ---------------------------------------------------------------------------
# Reading the design

class Design:
    """The schematic's netlist, and each part's footprint loaded from KiCad's libraries."""

    def __init__(self, netlist_path, footprints):
        with open(netlist_path, encoding='utf-8') as file:
            expr = sexpr.parse(file.read())
        self.parts = {}  # reference -> (value, footprint id, symbol path)
        for comp in find_all(find(expr, 'components'), 'comp'):
            sheet = find(find(comp, 'sheetpath'), 'tstamps')[1]
            path = sheet + find(comp, 'tstamps')[1]
            footprint = find(comp, 'footprint')
            self.parts[find(comp, 'ref')[1]] = (find(comp, 'value')[1], footprint[1] if footprint else '', path)
        self.nets = {}  # net name -> [(reference, pad number)]
        for net in find_all(find(expr, 'nets'), 'net'):
            name = find(net, 'name')[1]
            self.nets[name] = [(find(node, 'ref')[1], find(node, 'pin')[1]) for node in find_all(net, 'node')]
        self.footprints = {}
        for reference, (_, fpid, _) in self.parts.items():
            if not fpid:
                raise SystemExit('%s has no footprint' % reference)
            library, name = fpid.split(':')
            if fpid not in self.footprints:
                footprint = pcbnew.FootprintLoad(os.path.join(footprints, library + '.pretty'), name)
                if footprint is None:
                    raise SystemExit("can't load footprint %s" % fpid)
                self.footprints[fpid] = footprint

    def load(self, reference):
        """A fresh copy of a part's footprint."""
        return pcbnew.FOOTPRINT(self.footprints[self.parts[reference][1]])

    def kind(self, reference):
        if reference.startswith('J'):
            return 'header'
        if reference.startswith('C'):
            return 'bulk' if self.parts[reference][0] != '100nF' else 'decoupling'
        return 'chip'


def pad_offsets(footprint, angle):
    """Pad number -> offset (mm) from the footprint's body centre, rotated by angle, plus the body size."""
    footprint.SetOrientationDegrees(angle)
    footprint.SetPosition(pcbnew.VECTOR2I(0, 0))
    box = footprint.GetBoundingBox(False)
    centre = box.GetCenter()
    offsets = {}
    for pad in footprint.Pads():
        position = pad.GetPosition()
        offsets.setdefault(pad.GetNumber(), (pcbnew.ToMM(position.x - centre.x), pcbnew.ToMM(position.y - centre.y)))
    return offsets, (pcbnew.ToMM(box.GetWidth()), pcbnew.ToMM(box.GetHeight())), \
        (pcbnew.ToMM(-centre.x), pcbnew.ToMM(-centre.y))


# ---------------------------------------------------------------------------
# The anneal

class Placer:
    """Chips in slots. fixed maps (reference, pad) to a point that never moves, like a header pin."""

    def __init__(self, chips, offsets, nets, fixed, cols, rows, pitch, origin):
        self.chips, self.cols, self.rows, self.pitch, self.origin = chips, cols, rows, pitch, origin
        self.slots = cols * rows
        index = {chip: i for i, chip in enumerate(chips)}
        # A net is a list of (chip index or -1, dx, dy): a pin's offset from its slot centre, or a fixed point.
        self.nets = []
        for pins in nets:
            net = []
            for reference, pad in pins:
                if reference in index:
                    net.append((index[reference],) + offsets[reference][pad])
                elif (reference, pad) in fixed:
                    net.append((-1,) + fixed[(reference, pad)])
            if len(net) > 1:
                self.nets.append(net)
        self.chip_nets = [[] for _ in chips]
        for n, net in enumerate(self.nets):
            for chip in {pin[0] for pin in net if pin[0] >= 0}:
                self.chip_nets[chip].append(n)

    def centre(self, slot):
        return (self.origin[0] + (slot % self.cols + 0.5) * self.pitch[0],
                self.origin[1] + (slot // self.cols + 0.5) * self.pitch[1])

    def hpwl(self, n):
        xs, ys = [], []
        for chip, dx, dy in self.nets[n]:
            if chip >= 0:
                x, y = self.centres[self.slot_of[chip]]
                xs.append(x + dx)
                ys.append(y + dy)
            else:
                xs.append(dx)
                ys.append(dy)
        return max(xs) - min(xs) + max(ys) - min(ys)

    def anneal(self, rng, moves, start=None):
        self.centres = [self.centre(slot) for slot in range(self.slots)]
        self.slot_of = list(start) if start else rng.sample(range(self.slots), len(self.chips))
        occupant = [-1] * self.slots
        for chip, slot in enumerate(self.slot_of):
            occupant[slot] = chip
        costs = [self.hpwl(n) for n in range(len(self.nets))]
        cost = sum(costs)
        temperature = cost / max(1, len(self.nets)) * 2
        cooling = (1e-3 / temperature) ** (1 / moves) if moves and temperature > 1e-3 else 1
        for _ in range(moves):
            a = rng.randrange(len(self.chips))
            old_slot, new_slot = self.slot_of[a], rng.randrange(self.slots)
            temperature *= cooling
            if new_slot == old_slot:
                continue
            b = occupant[new_slot]
            touched = set(self.chip_nets[a]).union(self.chip_nets[b] if b >= 0 else ())
            before = sum(costs[n] for n in touched)
            self.slot_of[a], occupant[new_slot], occupant[old_slot] = new_slot, a, b
            if b >= 0:
                self.slot_of[b] = old_slot
            after = {n: self.hpwl(n) for n in touched}
            delta = sum(after.values()) - before
            if delta <= 0 or rng.random() < math.exp(-delta / temperature):
                for n, value in after.items():
                    costs[n] = value
                cost += delta
            else:  # undo
                self.slot_of[a], occupant[old_slot], occupant[new_slot] = old_slot, a, b
                if b >= 0:
                    self.slot_of[b] = new_slot
        return cost, list(self.slot_of)


# ---------------------------------------------------------------------------
# The board

class Board:
    def __init__(self, design, width, height):
        self.design = design
        self.board = pcbnew.BOARD()
        self.board.SetCopperLayerCount(4)
        self.netcodes = {}
        for name in design.nets:
            item = pcbnew.NETINFO_ITEM(self.board, name)
            self.board.Add(item)
            self.netcodes[name] = item
        self.pad_net = {(ref, pad): name for name, pins in design.nets.items() for ref, pad in pins}
        self.width, self.height = width, height
        self.outline()

    def outline(self):
        corners = [(0, 0), (self.width, 0), (self.width, self.height), (0, self.height)]
        for (x0, y0), (x1, y1) in zip(corners, corners[1:] + corners[:1]):
            segment = pcbnew.PCB_SHAPE(self.board)
            segment.SetShape(pcbnew.SHAPE_T_SEGMENT)
            segment.SetStart(pcbnew.VECTOR2I(mm(x0), mm(y0)))
            segment.SetEnd(pcbnew.VECTOR2I(mm(x1), mm(y1)))
            segment.SetLayer(pcbnew.Edge_Cuts)
            segment.SetWidth(mm(0.1))
            self.board.Add(segment)

    def place(self, reference, centre, angle):
        """Put a part's body centre at centre (mm)."""
        value, _, path = self.design.parts[reference]
        footprint = self.design.load(reference)
        footprint.SetFPIDAsString(self.design.parts[reference][1])
        footprint.SetReference(reference)
        footprint.SetValue(value)
        footprint.SetPath(pcbnew.KIID_PATH(path))
        footprint.SetOrientationDegrees(angle)
        _, _, shift = pad_offsets(footprint, angle)
        footprint.SetPosition(pcbnew.VECTOR2I(mm(centre[0] + shift[0]), mm(centre[1] + shift[1])))
        for pad in footprint.Pads():
            net = self.pad_net.get((reference, pad.GetNumber()))
            if net:
                pad.SetNet(self.netcodes[net])
        self.board.Add(footprint)
        return footprint

    def planes(self):
        for layer, plane in ((pcbnew.In1_Cu, 'GND'), (pcbnew.In2_Cu, 'VCC')):
            net = net_name(self.netcodes, plane)
            if not net:
                continue
            zone = pcbnew.ZONE(self.board)
            zone.SetLayer(layer)
            zone.SetNet(self.netcodes[net])
            zone.SetLocalClearance(mm(0.3))
            outline = zone.Outline()
            outline.NewOutline()
            for x, y in ((0, 0), (self.width, 0), (self.width, self.height), (0, self.height)):
                outline.Append(mm(x), mm(y))
            outline.Outline(0).SetClosed(True)
            self.board.Add(zone)

    def save(self, path):
        self.board.Save(path)
        fill_zones(path)


# ---------------------------------------------------------------------------

def place_project(directory, pitch=None, density=1.4, runs=6, moves=None, seed=1, cli=None, candidates=5):
    """Writes Candidates/1..N.kicad_pcb and <project>.kicad_pcb (a copy of the best); returns that path."""
    name = os.path.basename(os.path.normpath(directory))
    netlist = os.path.join(directory, name + '.net')
    subprocess.run([cli or kicad_cli(), 'sch', 'export', 'netlist', '--format', 'kicadsexpr', '-o', netlist,
                    os.path.join(directory, name + '.kicad_sch')], check=True, capture_output=True)
    design = Design(netlist, find_kicad('share/kicad/footprints'))
    kinds = {kind: sorted((r for r in design.parts if design.kind(r) == kind), key=reference_order)
             for kind in ('chip', 'decoupling', 'bulk', 'header')}
    chips = kinds['chip']

    offsets, sizes = {}, {}
    for reference in design.parts:
        angle = 90 if design.kind(reference) == 'header' else CHIP_ANGLE
        offsets[reference], sizes[reference], _ = pad_offsets(design.load(reference), angle)
    biggest = (max(sizes[c][0] for c in chips), max(sizes[c][1] for c in chips))
    pitch = pitch or (round(biggest[0] + 10, 1), round(biggest[1] + 10, 1))  # room for the cap and a channel
    if biggest[0] > pitch[0] or biggest[1] > pitch[1]:
        raise SystemExit('a %.1f x %.1f mm chip does not fit a %g x %g slot' % (biggest + tuple(pitch)))
    count = math.ceil(len(chips) * density)
    cols = max(1, round(math.sqrt(count * pitch[1] / pitch[0] * 1.6)))
    rows = math.ceil(count / cols)

    # Headers in a row along the bottom edge, then the bulk cap after them.
    header_height = max((sizes[h][1] for h in kinds['header']), default=0)
    width = max(2 * EDGE + cols * pitch[0], 2 * EDGE + sum(sizes[h][0] + 5 for h in kinds['header']) + 10)
    height = 2 * EDGE + rows * pitch[1] + (header_height + EDGE if kinds['header'] else 0)
    fixed_at, x = {}, EDGE
    for header in kinds['header']:
        fixed_at[header] = (x + sizes[header][0] / 2, height - EDGE - header_height / 2)
        x += sizes[header][0] + 5
    for cap in kinds['bulk']:
        fixed_at[cap] = (x + sizes[cap][0] / 2, height - EDGE - header_height / 2)
    fixed = {(ref, pad): (cx + dx, cy + dy) for ref, (cx, cy) in fixed_at.items() for pad, (dx, dy) in offsets[ref].items()}

    signal_nets = [pins for net, pins in design.nets.items() if net.lstrip('/') not in POWER]
    placer = Placer(chips, offsets, signal_nets, fixed, cols, rows, pitch, (EDGE, EDGE))
    moves = moves or 4000 * len(chips)
    rng = random.Random(seed)
    results = []
    for run in range(runs):
        cost, slots = placer.anneal(rng, moves)
        results.append((cost, slots))
        print('  anneal %d: wirelength %.0f mm' % (run + 1, cost))
    random_cost = sum(placer.anneal(random.Random(seed + run), 0)[0] for run in range(5)) / 5
    results.sort()
    print('%s: %d chips in %d x %d slots of %g x %g mm; best %.0f mm, random %.0f mm'
          % (name, len(chips), cols, rows, pitch[0], pitch[1], results[0][0], random_cost))

    def write(slots, path):
        write_board(design, kinds, sizes, offsets, placer, fixed_at, slots, width, height, path)
        print('  -> %s (%.0f x %.0f mm)' % (path, width, height))

    # The best few placements become candidates for route.py, which routes each and keeps the best.
    folder = os.path.join(directory, 'Candidates')
    os.makedirs(folder, exist_ok=True)
    for old in os.listdir(folder):
        os.remove(os.path.join(folder, old))
    for k, (_, slots) in enumerate(results[:candidates]):
        write(slots, os.path.join(folder, '%d.kicad_pcb' % (k + 1)))
    path = os.path.join(directory, name + '.kicad_pcb')
    write(results[0][1], path)
    return path


def write_board(design, kinds, sizes, offsets, placer, fixed_at, best, width, height, path):
    chips = kinds['chip']
    board = Board(design, width, height)
    vcc = {}  # chip -> its VCC pad, in mm
    for chip, slot in zip(chips, best):
        centre = placer.centre(slot)
        board.place(chip, centre, CHIP_ANGLE)
        # The VCC pad at the end of the chip: a '74 also ties preset and clear pins to VCC mid-row.
        pads = [offsets[chip][pad] for ref, pad in design.nets.get(net_name(design.nets, 'VCC'), []) if ref == chip]
        if pads:
            dx, dy = max(pads, key=lambda offset: (abs(offset[0]), -offset[1]))
            vcc[chip] = (centre[0] + dx, centre[1] + dy, centre)
    for reference, at in fixed_at.items():
        board.place(reference, at, 90 if design.kind(reference) == 'header' else CHIP_ANGLE)
    # Caps are interchangeable: one beside each chip's VCC pad, just past the end of the chip.
    caps = kinds['decoupling']
    if len(caps) < len(vcc):
        print('  warning: %d chips but %d decoupling caps' % (len(vcc), len(caps)))
    for cap, (chip, (x, y, centre)) in zip(caps, sorted(vcc.items(), key=lambda item: reference_order(item[0]))):
        outward = 1 if x >= centre[0] else -1
        board.place(cap, (x + outward * (CAP_GAP + sizes[cap][0] / 2), y), CHIP_ANGLE)
    for k, cap in enumerate(caps[len(vcc):]):  # spares, in a row from the bottom right corner
        board.place(cap, (width - EDGE - (k + 0.5) * (sizes[cap][0] + CAP_GAP), height - EDGE), CHIP_ANGLE)
    board.planes()
    board.save(path)


def reference_order(reference):
    letters = reference.rstrip('0123456789')
    return letters, int(reference[len(letters):] or 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('project', help='a project directory from kicad.py, e.g. "Build/KiCad/Register 16"')
    parser.add_argument('--pitch', help='slot size in mm, WxH (default: the biggest chip plus 10 mm each way)')
    parser.add_argument('--density', type=float, default=1.4, help='slots per chip (default 1.4)')
    parser.add_argument('--runs', type=int, default=6, help='independent anneals (default 6)')
    parser.add_argument('--candidates', type=int, default=5, help='best placements kept for routing (default 5)')
    parser.add_argument('--moves', type=int, help='moves per anneal (default 4000 per chip)')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--kicad-cli', help="exports the schematic's netlist (default: KiCad's own)")
    args = parser.parse_args()
    pitch = tuple(float(v) for v in args.pitch.lower().split('x')) if args.pitch else None
    place_project(args.project, pitch, args.density, args.runs, args.moves, args.seed, args.kicad_cli,
                  args.candidates)


if __name__ == '__main__':
    main()
