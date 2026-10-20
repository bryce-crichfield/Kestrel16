"""SVG drawing for datasheets: pinout symbols and orthogonal (right-angle) schematics.

All text is monospace so widths are exact: CHAR = 0.6 * font size.
"""
from html import escape

FONT = 11
CHAR = FONT * 0.6
STROKE = '#111'
BUS_W, WIRE_W = 2.6, 1.1


def tw(s, size=FONT):
    return len(s) * size * 0.6


def text(x, y, s, anchor='start', size=FONT, weight=400, fill=STROKE):
    return (f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" font-weight="{weight}" fill="{fill}" '
            f'text-anchor="{anchor}" dominant-baseline="central">{escape(s)}</text>')


def line(pts, width=WIRE_W):
    d = 'M' + ' L'.join(f'{x:.1f} {y:.1f}' for x, y in pts)
    return f'<path d="{d}" fill="none" stroke="{STROKE}" stroke-width="{width}" stroke-linejoin="miter"/>'


def dot(x, y, r=2.8):
    return f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{r}" fill="{STROKE}"/>'


def bubble(x, y, side):  # active-low circle sitting just outside a box edge
    cx = x - 4 if side == 'left' else x + 4
    return f'<circle cx="{cx:.1f}" cy="{y:.1f}" r="4" fill="#fff" stroke="{STROKE}" stroke-width="1"/>'


def clock_mark(x, y):  # edge-trigger triangle just inside a left edge
    return line([(x, y - 6), (x + 8, y), (x, y + 6)], 1)


def bus_slash(x, y, width):
    return line([(x - 4, y + 6), (x + 4, y - 6)], 1) + text(x, y - 13, str(width), 'middle', 9)


def flag(x, y, label, side):
    """Port flag (pentagon). side='in' points right with its tip at x; 'out' starts at x and points right."""
    w, h = tw(label) + 18, 18
    if side == 'in':
        x0, x1 = x - w, x - 8
        pts = [(x0, y - h / 2), (x1, y - h / 2), (x, y), (x1, y + h / 2), (x0, y + h / 2)]
        tx = (x0 + x1) / 2
    else:
        x0, x1 = x, x + w - 8
        pts = [(x0, y - h / 2), (x1, y - h / 2), (x + w, y), (x1, y + h / 2), (x0, y + h / 2)]
        tx = (x0 + x1) / 2
    p = ' '.join(f'{a:.1f},{b:.1f}' for a, b in pts)
    return (f'<polygon points="{p}" fill="#fff" stroke="{STROKE}" stroke-width="1"/>' +
            text(tx, y, label, 'middle'))


def svg(w, h, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w:.0f} {h:.0f}" '
            f'font-family="DejaVu Sans Mono, Menlo, Consolas, monospace">{"".join(body)}</svg>')


# ---------------------------------------------------------------- pinout

def pinout(name, pins):
    ins = [p for p in pins if p['dir'] == 'in']
    outs = [p for p in pins if p['dir'] != 'in']
    pitch, stub, box_w = 30, 56, 210
    n = max(len(ins), len(outs))
    box_h = n * pitch + 30
    label_w = max(tw(p['name']) for p in pins) + 10
    bx = label_w + stub
    by = 20
    W, H = bx + box_w + stub + label_w, by + box_h + 20
    body = [f'<rect x="{bx}" y="{by}" width="{box_w}" height="{box_h}" fill="#fff" stroke="{STROKE}" stroke-width="1.4"/>',
            text(bx + box_w / 2, by + box_h / 2, name, 'middle', 13, 700)]

    def side(group, left):
        gap = box_h / len(group)
        for i, p in enumerate(group):
            y = by + gap * (i + 0.5)
            wide = p.get('width', 1) > 1
            edge = bx if left else bx + box_w
            far = edge - stub if left else edge + stub
            near = edge - (8 if p.get('active') == 'low' else 0) if left else edge + (8 if p.get('active') == 'low' else 0)
            body.append(line([(far, y), (near, y)], BUS_W if wide else WIRE_W))
            if p.get('active') == 'low':
                body.append(bubble(edge, y, 'left' if left else 'right'))
            if p.get('clock'):
                body.append(clock_mark(edge, y))
            if wide:
                body.append(bus_slash((far + edge) / 2, y, p['width']))
            body.append(text(far - 6, y, p['name'], 'end') if left else text(far + 6, y, p['name']))

    side(ins, True)
    side(outs, False)
    return svg(W, H, body)


# ---------------------------------------------------------------- schematic

PITCH, CHIP_W, HEAD, FOOT, VGAP, TRACK = 22, 128, 30, 8, 46, 14


def schematic(spec):
    """spec: chips {id: {part, label, inputs:[pin], outputs:[pin]}}, columns [[id...]],
    nets [{from, to:[...], width}]. Endpoints are 'U1.Q' (chip pin) or 'D[7:0]' (component port).
    A pin is a string, or {name, width, active: low, clock: true, side: top|bottom}.
    Top/bottom pins sit near the right of the chip; a bottom pin wired to the top pin of the
    chip below it in the same column (a carry chain) is drawn as a straight vertical link."""
    chips, cols, nets = spec['chips'], spec['columns'], spec['nets']
    norm = lambda p: p if isinstance(p, dict) else {'name': p}
    col_of = {c: i for i, col in enumerate(cols) for c in col}

    def is_pin(e):
        return '.' in e and e.split('.')[0] in chips

    # tracks needed in each channel (channel c sits left of column c; channel len(cols) is the output side)
    chan = [[] for _ in range(len(cols) + 1)]
    for n in nets:
        dcols = {col_of[d.split('.')[0]] for d in n['to'] if is_pin(d)}
        if is_pin(n['from']) and dcols == {col_of[n['from'].split('.')[0]]}:
            n['_chain'] = True
            continue
        assert len(dcols) <= 1, f'net {n["from"]} fans into several columns'
        n['_col'] = dcols.pop() if dcols else len(cols)
        if n['_col'] < len(cols):
            chan[n['_col']].append(n)

    # chip geometry
    size = {}
    for cid, c in chips.items():
        c['inputs'] = [norm(p) for p in c.get('inputs', [])]
        c['outputs'] = [norm(p) for p in c.get('outputs', [])]
        rows = max(len([p for p in c['inputs'] if 'side' not in p]),
                   len([p for p in c['outputs'] if 'side' not in p]))
        has_bottom = any(p.get('side') == 'bottom' for p in c['outputs'])
        size[cid] = HEAD + rows * PITCH + FOOT + (14 if has_bottom else 0)
    col_h = [sum(size[c] for c in col) + VGAP * (len(col) - 1) for col in cols]
    top = 30

    in_ports = [n['from'] for n in nets if not is_pin(n['from'])]
    out_ports = [d for n in nets for d in n['to'] if not is_pin(d)]
    left_w = max([tw(p) + 30 for p in in_ports] + [40])
    right_w = max([tw(p) + 30 for p in out_ports] + [40])

    x = left_w
    chan_x, col_x = [], []
    for c in range(len(cols)):
        chan_x.append(x)
        x += 34 + TRACK * max(len(chan[c]) - 1, 0) + 30
        col_x.append(x)
        x += CHIP_W
    chan_x.append(x)
    out_edge = x + 50
    W = out_edge + right_w

    pin_xy, entry, body = {}, {}, []
    tallest = max(col_h)
    for ci, col in enumerate(cols):
        y = top + (tallest - col_h[ci]) / 2
        for cid in col:
            c, h, cx = chips[cid], size[cid], col_x[ci]
            body.append(f'<rect x="{cx}" y="{y:.1f}" width="{CHIP_W}" height="{h}" fill="#fff" stroke="{STROKE}" stroke-width="1.4"/>')
            if any(p.get('side') == 'top' for p in c['inputs']):
                body.append(text(cx + 6, y + 12, c['part'], 'start', 11, 700))
            else:
                body.append(text(cx + CHIP_W / 2, y + 12, c['part'], 'middle', 11, 700))
            body.append(text(cx, y - 9, cid, 'start', 10, 700))
            if c.get('label'):
                body.append(text(cx + tw(cid, 10) + 8, y - 9, c['label'], 'start', 10, 400, '#555'))
            body.append(line([(cx, y + 22), (cx + CHIP_W, y + 22)], 0.6))
            for p in c['inputs'] + c['outputs']:
                if p.get('side') in ('top', 'bottom'):
                    px = cx + CHIP_W - 26
                    py = y if p['side'] == 'top' else y + h
                    body.append(text(px, y + 12 if p['side'] == 'top' else y + h - 10, p['name'], 'middle', 10))
                    pin_xy[f'{cid}.{p["name"]}'] = (px, py)
                    entry[f'{cid}.{p["name"]}'] = py - 22 if p['side'] == 'top' else py + 22
            for side, group in (('in', c['inputs']), ('out', c['outputs'])):
                group = [p for p in group if 'side' not in p]
                for i, p in enumerate(group):
                    py = y + HEAD + PITCH * (i + 0.5)
                    ex = cx if side == 'in' else cx + CHIP_W
                    low = p.get('active') == 'low'
                    body.append(text(ex + (10 if p.get('clock') else 5) if side == 'in' else ex - 5,
                                     py, p['name'], 'start' if side == 'in' else 'end', 10))
                    if low:
                        body.append(bubble(ex, py, 'left' if side == 'in' else 'right'))
                    if p.get('clock'):
                        body.append(clock_mark(ex, py))
                    pin_xy[f'{cid}.{p["name"]}'] = (ex - 8 if low and side == 'in' else ex + 8 if low else ex, py)
            y += h + VGAP

    bottom = top + tallest
    lane = bottom + 22
    lines, dots, flags = [], [], []

    for c, group in enumerate(chan[:len(cols)]):
        group.sort(key=lambda n: (not is_pin(n['from']), min(pin_xy[d][1] for d in n['to'])))
        for k, n in enumerate(group):
            n['_tx'] = chan_x[c] + 34 + TRACK * k

    for n in nets:
        if n.get('_chain'):
            (ax, ay), (bx, by) = pin_xy[n['from']], pin_xy[n['to'][0]]
            assert abs(ax - bx) < 0.5, 'chain pins must line up'
            lines.append(line([(ax, ay), (bx, by)]))
            continue
        wdt = BUS_W if n.get('width', 1) > 1 else WIRE_W
        dests = [pin_xy[d] if is_pin(d) else None for d in n['to']]
        ends = {i: entry[d] for i, d in enumerate(n['to']) if d in entry}  # top pins: approach from above
        reach = [d and (d[0], ends.get(i, d[1])) for i, d in enumerate(dests)]
        src = n['from']
        if not is_pin(src):  # component input port
            first = min(d[1] for d in reach)
            skip = n['_col'] > 0
            sy = lane if skip else first
            if skip:
                lane += 22
            sx = left_w
            flags.append(flag(sx, sy, src, 'in'))
        else:
            sx, sy = pin_xy[src]
        if n['_col'] == len(cols):  # straight out to an output port
            lines.append(line([(sx, sy), (out_edge, sy)], wdt))
            flags.append(flag(out_edge, sy, n['to'][0], 'out'))
            if n.get('width', 1) > 1:
                lines.append(bus_slash(sx + 24, sy, n['width']))
            continue
        tx = n['_tx']
        if len(dests) == 1 and not ends and abs(dests[0][1] - sy) < 0.5:
            lines.append(line([(sx, sy), dests[0]], wdt))
        else:
            ys = [sy] + [r[1] for r in reach]
            lines.append(line([(sx, sy), (tx, sy)], wdt))
            lines.append(line([(tx, min(ys)), (tx, max(ys))], wdt))
            for i, (d, r) in enumerate(zip(dests, reach)):
                lines.append(line([(tx, r[1]), (d[0], r[1]), d] if i in ends else [(tx, d[1]), d], wdt))
            dests = reach
            lo, hi = min(ys), max(ys)
            for yy in sorted(set(round(v, 1) for v in ys)):
                left = abs(yy - sy) < 0.5
                right = any(abs(yy - d[1]) < 0.5 for d in dests)
                if left + right + (yy > lo + 0.5) + (yy < hi - 0.5) >= 3:  # a T-junction
                    dots.append(dot(tx, yy))
        if n.get('width', 1) > 1:
            lines.append(bus_slash(sx + 14, sy, n['width']))

    H = lane + 10
    return svg(W, H, lines + body + dots + flags)
