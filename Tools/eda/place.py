"""Places a KiCad project's chips into a slot grid by simulated annealing and writes the board.

Run it with KiCad's own Python, which has the pcbnew module:

  "C:/Program Files/KiCad/9.0/bin/python.exe" Tools/eda/place.py "Build/KiCad/Register 16"

It reads the netlist kicad-cli exports from the schematic, freshly each run, so it sees exactly what the schematic
has. Each chip gets a slot sized from its own footprint plus --gap, and the slots, with spares so chips can move,
are packed onto the board in aligned rows, or packed freely (short chips stacked beside tall ones) when rows are too tight. A chip moves only between slots of its own size.
The anneal swaps or moves chips to shorten the weighted half-perimeter of every signal net; GND and VCC are skipped,
since they are the inner planes. Several independent anneals run. Then each 100nF cap goes beside a chip's VCC pin,
the headers go along the bottom edge with the bulk cap (from the connector's side), the chips are centred on the
board, and the board is written as <project>.kicad_pcb: 4 layers,
In1 a GND plane and In2 a VCC plane (or, with --signal-layers 3, a third signal layer), ready for routing. The best few placements are also written as
Candidates/1.kicad_pcb, 2, ... for route.py, which routes each and keeps the best.

With --inflate N, up to N more rounds follow: congestion.py's estimate (pads blocking) of the best placement finds
the chips whose slots are busy, their gaps grow a step (to at most 1.5 times), and the chips are packed and annealed
again. It stops when no chip is busy, the grown slots don't pack, or the congestion stops improving, and keeps the
least congested round. Spares are what pays for the room, so a full board grows little.
"""
import argparse
import json
import math
import os
import random
import re
import shutil
import subprocess
import sys
import time
from collections import namedtuple

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr  # noqa: E402
from congestion import BUSY, rudy, via_box  # noqa: E402
from kicad_env import POWER, fill_zones, find_kicad, kicad_cli, net_name  # noqa: E402
from sexpr import find, find_all  # noqa: E402

EDGE = 5.0       # mm from the board edge to the first slot or header
CAP_GAP = 2.5    # mm from a VCC pad to its cap
CHIP_ANGLE = 90  # locked rotation: every chip lies on its side, pin 1 bottom left
UPRIGHT = 0      # with --vertical: every chip stands up, pin 1 top left
PAD = 1.6        # mm, a template pad (the backplane connector's, through hole), for congestion's blockage
GROW_STEP = 0.25  # --inflate grows a congested chip's gap by this much of itself each round
MOST_GROWN = 1.5  # up to this many times its gap
LEAST_GROWN = 0.75  # and a quiet chip's gap across it shrinks to no less than this, to pay for it


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
        self.blocks = {}  # reference -> the sheets it sits in, like "Shifter 16 > Bus Driver 16"
        self.sheet_of = {}  # reference -> its sheet path as tstamps, unique to each instance
        for comp in find_all(find(expr, 'components'), 'comp'):
            sheet = find(find(comp, 'sheetpath'), 'tstamps')[1]
            path = sheet + find(comp, 'tstamps')[1]
            footprint = find(comp, 'footprint')
            self.sheet_of[find(comp, 'ref')[1]] = sheet
            self.parts[find(comp, 'ref')[1]] = (find(comp, 'value')[1], footprint[1] if footprint else '', path)
            names = find(find(comp, 'sheetpath'), 'names')[1]
            named = [f[2] for f in find_all(find(comp, 'fields') or [], 'field') if len(f) > 2 and find(f, 'name')[1] == 'Block']
            self.blocks[find(comp, 'ref')[1]] = block(names + (named[0] + '/' if named else ''))
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
        if reference.startswith(('J', 'P')):  # headers and the backplane connector
            return 'header'
        if reference.startswith('C'):
            return 'bulk' if self.parts[reference][0] != '100nF' else 'decoupling'
        return 'chip'


def block(names, depth=3):
    """A sheet path, "/Registers/R3/Port A/Low byte/", as its innermost depth sheets without kicad.py's instance
    numbers: "R3 > Port A > Low byte". Sheets are named by hdl's Part(..., name=), or by their component; a chip
    kicad.py drew without a sheet brings its own name as its Block field. Empty at the top."""
    sheets = [re.sub(r' #\d+$', '', name) for name in names.strip('/').split('/') if name]
    return ' > '.join(sheets[-depth:])


def pad_offsets(footprint, angle):
    """Pad number -> offset (mm) from the footprint's body centre, rotated by angle, plus the body size, the shift
    from body centre to footprint origin and pad number -> (width, height, through) in mm, through being a pad that
    blocks every layer (not a surface one)."""
    footprint.SetOrientationDegrees(angle)
    footprint.SetPosition(pcbnew.VECTOR2I(0, 0))
    box = footprint.GetBoundingBox(False)
    centre = box.GetCenter()
    offsets, shapes = {}, {}
    for pad in footprint.Pads():
        position, area = pad.GetPosition(), pad.GetBoundingBox()
        offsets.setdefault(pad.GetNumber(), (pcbnew.ToMM(position.x - centre.x), pcbnew.ToMM(position.y - centre.y)))
        shapes.setdefault(pad.GetNumber(), (pcbnew.ToMM(area.GetWidth()), pcbnew.ToMM(area.GetHeight()),
                                            pad.GetAttribute() != pcbnew.PAD_ATTRIB_SMD))
    return offsets, (pcbnew.ToMM(box.GetWidth()), pcbnew.ToMM(box.GetHeight())), \
        (pcbnew.ToMM(-centre.x), pcbnew.ToMM(-centre.y)), shapes


# ---------------------------------------------------------------------------
# The anneal

def net_weight(pins):
    """Small nets count fully, a k-pin net about 2/(k-1), so buses and clocks that span the board whatever the
    placement don't drown out local wiring."""
    return 1.0 if pins <= 3 else 2.0 / (pins - 1)


def hierarchy_groups(design, chips, largest=0.5):
    """(members, weight) for every sheet holding two or more chips, like R3 or R3's Port A: a pull that keeps a
    block's chips together. Smaller blocks pull tighter. A sheet holding more than largest of the card's chips
    (the card's top block) is left out: it would pull everything toward everything."""
    found = {}
    for chip in chips:
        sheets = [s for s in design.sheet_of[chip].strip('/').split('/') if s]
        for depth in range(1, len(sheets) + 1):
            found.setdefault('/'.join(sheets[:depth]), []).append(chip)
    return [(members, 1.0 / math.sqrt(len(members))) for members in found.values()
            if 1 < len(members) <= largest * len(chips)]


class Placer:
    """Chips in slots. fixed maps (reference, pad) to a point that never moves, like a header pin.
    Slots come in sizes: classes[i] is chip i's slot size, and slot_classes[s] is slot s's. A chip only
    moves to a slot of its own size."""

    def __init__(self, chips, offsets, nets, fixed, centres, classes, slot_classes, groups=(), cohesion=1.0):
        self.chips, self.centres = chips, centres
        self.slots = len(centres)
        self.classes = classes
        self.class_slots = {}
        for slot, size in enumerate(slot_classes):
            self.class_slots.setdefault(size, []).append(slot)
        index = {chip: i for i, chip in enumerate(chips)}
        # A net is a list of (chip index or -1, dx, dy): a pin's offset from its slot centre, or a fixed point.
        self.nets, self.weights = [], []
        for pins in nets:
            net = []
            for reference, pad in pins:
                if reference in index:
                    net.append((index[reference],) + offsets[reference][pad])
                elif (reference, pad) in fixed:
                    net.append((-1,) + fixed[(reference, pad)])
            if len(net) > 1:
                self.nets.append(net)
                self.weights.append(net_weight(len(net)))
        self.signals = len(self.nets)  # the real nets; after them, one virtual net per block joining its chips
        if cohesion:
            for members, weight in groups:
                self.nets.append([(index[chip], 0.0, 0.0) for chip in members])
                self.weights.append(weight * cohesion)
        self.chip_nets = [[] for _ in chips]
        for n, net in enumerate(self.nets):
            for chip in {pin[0] for pin in net if pin[0] >= 0}:
                self.chip_nets[chip].append(n)

    def centre(self, slot):
        return self.centres[slot]

    def hpwl(self, n, weighted=True):
        xs, ys = [], []
        for chip, dx, dy in self.nets[n]:
            if chip >= 0:
                x, y = self.centres[self.slot_of[chip]]
                xs.append(x + dx)
                ys.append(y + dy)
            else:
                xs.append(dx)
                ys.append(dy)
        return (self.weights[n] if weighted else 1) * (max(xs) - min(xs) + max(ys) - min(ys))

    def wirelength(self, slots):
        """The plain half-perimeter wirelength (mm) of the real nets, unweighted, for a placement."""
        self.slot_of = list(slots)
        return sum(self.hpwl(n, False) for n in range(self.signals))

    def anneal(self, rng, moves, start=None):
        if start:
            self.slot_of = list(start)
        else:  # each size's chips in a random sample of that size's slots
            self.slot_of = [None] * len(self.chips)
            for size, slots in self.class_slots.items():
                members = [chip for chip, c in enumerate(self.classes) if c == size]
                for chip, slot in zip(members, rng.sample(slots, len(members))):
                    self.slot_of[chip] = slot
        occupant = [-1] * self.slots
        for chip, slot in enumerate(self.slot_of):
            occupant[slot] = chip
        costs = [self.hpwl(n) for n in range(len(self.nets))]
        cost = sum(costs)
        temperature = cost / max(1, len(self.nets)) * 2
        cooling = (1e-3 / temperature) ** (1 / moves) if moves and temperature > 1e-3 else 1
        for _ in range(moves):
            a = rng.randrange(len(self.chips))
            old_slot, new_slot = self.slot_of[a], rng.choice(self.class_slots[self.classes[a]])
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

class Template:
    """A board to place into, such as templates/6U_Eurocard/6U_Eurocard.kicad_pcb. Its Edge.Cuts outline sets the size, and
    the footprints on it (the backplane connector, mounting holes) stay where they are, with chips kept clear.
    A template footprint whose reference is also in the design (the connector, once a card's netlist has one)
    takes that part's nets, and its pads pull the chips they connect to."""

    def __init__(self, path):
        root, extension = os.path.splitext(os.path.abspath(path))
        self.path = root + '.kicad_pcb' if extension == '.kicad_pro' else root + extension  # a project's board
        board = pcbnew.LoadBoard(self.path) if os.path.isfile(self.path) else None
        if board is None:
            raise SystemExit("can't load the template board %s" % self.path)
        box = board.GetBoardEdgesBoundingBox()
        if box.GetWidth() == 0:
            raise SystemExit('%s has no Edge.Cuts outline' % path)
        self.origin = (pcbnew.ToMM(box.GetX()), pcbnew.ToMM(box.GetY()))
        self.size = (pcbnew.ToMM(box.GetWidth()), pcbnew.ToMM(box.GetHeight()))
        ox, oy = self.origin
        self.keepouts, self.pads, self.references, self.fpids = [], {}, set(), {}
        for footprint in board.GetFootprints():
            self.fpids[footprint.GetReference()] = footprint.GetFPIDAsString()
            body = footprint.GetBoundingBox(False)
            self.keepouts.append((pcbnew.ToMM(body.GetLeft()) - ox, pcbnew.ToMM(body.GetTop()) - oy,
                                  pcbnew.ToMM(body.GetRight()) - ox, pcbnew.ToMM(body.GetBottom()) - oy))
            reference = footprint.GetReference()
            self.references.add(reference)
            for pad in footprint.Pads():
                at = pad.GetPosition()
                self.pads[(reference, pad.GetNumber())] = (pcbnew.ToMM(at.x) - ox, pcbnew.ToMM(at.y) - oy)
        # Rule areas that keep out footprints, like a fanout zone above the connectors: KiCad's own keepouts, so DRC
        # checks them too. Each counts as its bounding box.
        self.zones = []
        for zone in board.Zones():
            if zone.GetIsRuleArea() and zone.GetDoNotAllowFootprints():
                box = zone.GetBoundingBox()
                self.zones.append((pcbnew.ToMM(box.GetLeft()) - ox, pcbnew.ToMM(box.GetTop()) - oy,
                                   pcbnew.ToMM(box.GetRight()) - ox, pcbnew.ToMM(box.GetBottom()) - oy))
        self.keepouts += self.zones

    def match(self, design):
        """{design reference: template reference} for the design's parts already on the template, like the
        backplane connector: the same reference, or else the same footprint (by library name, then by name)."""
        alias = {ref: ref for ref in design.parts if ref in self.references}
        for ref, (_, fpid, _) in sorted(design.parts.items()):
            if ref in alias or design.kind(ref) != 'header':
                continue
            free = [t for t in sorted(self.fpids) if t not in alias.values()]
            found = [t for t in free if self.fpids[t] == fpid] or \
                [t for t in free if self.fpids[t].split(':')[-1] == fpid.split(':')[-1]]
            if found:
                alias[ref] = found[0]
        return alias

    def clear(self, x0, y0, x1, y1, margin=2.0):
        """True if the rectangle stays margin mm from every template footprint."""
        return all(x1 + margin <= k[0] or x0 >= k[2] + margin or y1 + margin <= k[1] or y0 >= k[3] + margin
                   for k in self.keepouts)


class Board:
    def __init__(self, design, width, height, template=None, alias=None):
        self.design = design
        self.origin = template.origin if template else (0, 0)
        self.board = pcbnew.LoadBoard(template.path) if template else pcbnew.BOARD()
        self.board.SetCopperLayerCount(4)
        self.netcodes = {}
        for name in design.nets:
            item = pcbnew.NETINFO_ITEM(self.board, name)
            self.board.Add(item)
            self.netcodes[name] = item
        self.pad_net = {(ref, pad): name for name, pins in design.nets.items() for ref, pad in pins}
        self.width, self.height = width, height
        if template:
            design_ref = {t: d for d, t in (alias or {}).items()}
            for footprint in self.board.GetFootprints():
                ref = design_ref.get(footprint.GetReference())
                if ref is None:
                    continue
                # It becomes the schematic's part, so DRC's schematic parity sees one connector, not two.
                footprint.SetReference(ref)
                footprint.SetPath(pcbnew.KIID_PATH(design.parts[ref][2]))
                for pad in footprint.Pads():
                    net = self.pad_net.get((ref, pad.GetNumber()))
                    if net:
                        pad.SetNet(self.netcodes[net])
        else:
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
        _, _, shift, _ = pad_offsets(footprint, angle)
        x, y = centre[0] + shift[0] + self.origin[0], centre[1] + shift[1] + self.origin[1]
        footprint.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        for pad in footprint.Pads():
            net = self.pad_net.get((reference, pad.GetNumber()))
            if net:
                pad.SetNet(self.netcodes[net])
        self.board.Add(footprint)
        return footprint

    def label(self, text, corner='auto', angle=0, size=3.0):
        """The card's name in silkscreen, in a corner clear of every footprint: the preferred corner if it's clear,
        else the first clear one of top left, top right, bottom left and bottom right."""
        label = pcbnew.PCB_TEXT(self.board)
        label.SetText(text)
        label.SetLayer(pcbnew.F_SilkS)
        label.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
        label.SetTextThickness(mm(size * 0.15))
        label.SetTextAngleDegrees(angle)
        label.SetPosition(pcbnew.VECTOR2I(0, 0))
        box = label.GetBoundingBox()
        w, h = pcbnew.ToMM(box.GetWidth()), pcbnew.ToMM(box.GetHeight())
        cx, cy = pcbnew.ToMM(box.GetCenter().x), pcbnew.ToMM(box.GetCenter().y)
        taken = [footprint.GetBoundingBox(False) for footprint in self.board.GetFootprints()]
        taken = [tuple(pcbnew.ToMM(v) for v in (b.GetLeft(), b.GetTop(), b.GetRight(), b.GetBottom())) for b in taken]
        margin = 2.0  # mm clear of the edge and of footprints
        ox, oy = self.origin
        spots = {'top-left': (margin + w / 2, margin + h / 2),
                 'top-right': (self.width - margin - w / 2, margin + h / 2),
                 'bottom-left': (margin + w / 2, self.height - margin - h / 2),
                 'bottom-right': (self.width - margin - w / 2, self.height - margin - h / 2)}
        order = ([corner] if corner in spots else []) + [c for c in spots if c != corner]
        for name in order:
            x, y = spots[name][0] + ox, spots[name][1] + oy
            if not overlaps(x - w / 2 - margin, y - h / 2 - margin, x + w / 2 + margin, y + h / 2 + margin, taken):
                break
        else:
            name = order[0]
            x, y = spots[name][0] + ox, spots[name][1] + oy
            print('  warning: no corner is clear for the label, so it overlaps parts at the %s' % name)
        if corner in spots and name != corner:
            print('  the %s corner is taken, so the label goes at the %s' % (corner, name))
        label.SetPosition(pcbnew.VECTOR2I(mm(x - cx), mm(y - cy)))
        self.board.Add(label)

    def mark(self, footprint, centre, angle, lines, length, back=False):
        """Silkscreen lines on a chip's body, like its part number and the block it belongs to, along the body and
        shrunk to fit its length; with back, mirrored on the back too, where the chips' names help when probing."""
        centre = pcbnew.VECTOR2I(mm(centre[0] + self.origin[0]), mm(centre[1] + self.origin[1]))
        lines = [line for line in lines if line]
        for layer in (pcbnew.F_SilkS, pcbnew.B_SilkS) if back else (pcbnew.F_SilkS,):
            text = pcbnew.PCB_TEXT(footprint)
            text.SetText('\n'.join(lines))
            text.SetLayer(layer)
            text.SetMirrored(layer == pcbnew.B_SilkS)
            size = 1.0
            while True:
                text.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
                text.SetTextThickness(mm(size * 0.15))
                text.SetTextAngleDegrees(0)
                if pcbnew.ToMM(text.GetBoundingBox().GetWidth()) <= length * 0.9 or size <= 0.5:
                    break
                size -= 0.1
            footprint.Add(text)
            text.SetTextAngleDegrees(angle)
            text.SetPosition(centre)

    def planes(self, signal_layers=2):
        """In1 a GND plane, and In2 a VCC plane unless it's a third signal layer, VCC then being routed."""
        planes = ((pcbnew.In1_Cu, 'GND'), (pcbnew.In2_Cu, 'VCC'))[:4 - signal_layers]
        for layer, plane in planes:
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
                outline.Append(mm(x + self.origin[0]), mm(y + self.origin[1]))
            outline.Outline(0).SetClosed(True)
            self.board.Add(zone)

    def save(self, path):
        self.board.Save(path)
        fill_zones(path)


# ---------------------------------------------------------------------------

def slot_size(size, gap):
    """A chip's slot: its body plus the gap (room for its cap and a routing channel), rounded up to 0.5 mm."""
    return tuple(math.ceil((size[i] + gap[i]) * 2) / 2 for i in (0, 1))


ROOMY = 1.2  # rows are kept when they leave at least this many slots per chip (or as many as free packing does)


def overlaps(x0, y0, x1, y1, obstacles):
    return any(x0 < k[2] and x1 > k[0] and y0 < k[3] and y1 > k[1] for k in obstacles)


def rows(slots, left, top, right, bottom, obstacles=()):
    """Shelf-packs slot sizes into the rectangle, tallest first, left to right in aligned rows: the tidy layout.
    Each row is as tall as its tallest slot, which wastes room under short ones. Returns [(size, centre)], or
    None if they don't all fit."""
    placed, x, y, row = [], left, top, 0
    for w, h in sorted(slots, key=lambda s: (-s[1], -s[0])):
        while True:
            if row == 0:
                row = h
            if y + h > bottom:
                return None
            spot = x
            while spot + w <= right and overlaps(spot, y, spot + w, y + h, obstacles):
                spot += 1
            if spot + w <= right:
                placed.append(((w, h), (spot + w / 2, y + row / 2)))
                x = spot + w
                break
            x, y, row = left, y + row, 0  # the next row
    return placed


def pack(slots, left, top, right, bottom, obstacles=()):
    """Packs slot sizes into the rectangle by MaxRects, for when rows don't fit: it keeps every maximal free
    rectangle, and puts each slot,
    biggest first, at the top-most then left-most free spot it fits. Short slots stack beside and under tall ones,
    so nothing is lost to row heights. obstacles are (x0, y0, x1, y1) rectangles to keep clear, like a template's
    connector. Returns [(size, centre)], or None if they don't all fit."""
    free = [(left, top, right, bottom)]
    for obstacle in obstacles:
        free = carve(free, obstacle)
    placed = []
    for w, h in sorted(slots, key=lambda s: (-s[0] * s[1], -s[1])):
        fits = [(y0, x0) for x0, y0, x1, y1 in free if x1 - x0 >= w - 1e-6 and y1 - y0 >= h - 1e-6]
        if not fits:
            return None
        y, x = min(fits)
        placed.append(((w, h), (x + w / 2, y + h / 2)))
        free = carve(free, (x, y, x + w, y + h))
    return placed


def carve(free, used):
    """The maximal free rectangles left once used is taken out of each of free's."""
    ux0, uy0, ux1, uy1 = used
    pieces = []
    for x0, y0, x1, y1 in free:
        if ux0 >= x1 or ux1 <= x0 or uy0 >= y1 or uy1 <= y0:
            pieces.append((x0, y0, x1, y1))
            continue
        for piece in ((x0, y0, ux0, y1), (ux1, y0, x1, y1), (x0, y0, x1, uy0), (x0, uy1, x1, y1)):
            if piece[2] - piece[0] > 1e-6 and piece[3] - piece[1] > 1e-6:
                pieces.append(piece)
    return [a for i, a in enumerate(pieces)  # drop any piece inside another (keeping the first of duplicates)
            if not any(j != i and b[0] <= a[0] and b[1] <= a[1] and b[2] >= a[2] and b[3] >= a[3]
                       and (b != a or j < i) for j, b in enumerate(pieces))]


def centring(chips, region, obstacles):
    """How far to move every chip, given as (centre, slot size), so that together they sit centred on the board.
    Across, the aim is the middle of region; down, the middle of the span left beside obstacles under or over the
    chips (the connectors along the bottom edge). The shift taken is the one nearest that aim, on a 0.5 mm grid,
    that keeps every chip inside region and clear of the obstacles. Only chips count, so spare slots and the bulk
    cap in the bottom strip don't pull them off centre."""
    boxes = [(x - w / 2, y - h / 2, x + w / 2, y + h / 2) for (x, y), (w, h) in chips]
    x0, y0 = min(b[0] for b in boxes), min(b[1] for b in boxes)
    x1, y1 = max(b[2] for b in boxes), max(b[3] for b in boxes)
    left, top, right, bottom = region
    for k in obstacles:
        if k[0] < x1 and k[2] > x0:  # under or over the chips: it narrows them down
            top, bottom = (max(top, k[3]), bottom) if k[1] + k[3] < y0 + y1 else (top, min(bottom, k[1]))
    aim = ((left + right - x0 - x1) / 2, (top + bottom - y0 - y1) / 2)

    def steps(low, high):
        return [k / 2 for k in range(math.ceil(low * 2), math.floor(high * 2) + 1)]

    shifts = [(dx, dy) for dx in steps(region[0] - x0, region[2] - x1) for dy in steps(region[1] - y0, region[3] - y1)]
    for dx, dy in sorted(shifts + [(0, 0)], key=lambda d: (d[0] - aim[0]) ** 2 + (d[1] - aim[1]) ** 2):
        if (dx, dy) == (0, 0) or not any(overlaps(b[0] + dx, b[1] + dy, b[2] + dx, b[3] + dy, obstacles)
                                         for b in boxes):
            return dx, dy
    return 0, 0


Layout = namedtuple('Layout', 'width height fixed fixed_at placer results slot_of')  # one round's placements


def congestion_of(design, chips, offsets, shapes, layout, signal_layers, tile=2.54):
    """congestion.py's estimate of a layout's best placement, pads blocking, without writing a board:
    (overflow, peak, {chip: the 90th percentile utilisation over its slot}). shapes: reference -> pad -> (width,
    height, through), as pad_offsets gives them; a pad with none, the template's, is a PAD square through hole."""
    placer, (_, slots, shift) = layout.placer, layout.results[0]
    at = dict(layout.fixed)
    for chip, slot in zip(chips, slots):
        x, y = (c + d for c, d in zip(placer.centre(slot), shift))
        at.update({(chip, pad): (x + dx, y + dy) for pad, (dx, dy) in offsets[chip].items()})
    def shape(pin):
        return shapes.get(pin[0], {}).get(pin[1], (PAD, PAD, True))

    nets, pads = {}, []
    for net, pins in design.nets.items():
        points = [at[pin] for pin in pins if pin in at]
        if net.lstrip('/') not in POWER[:4 - signal_layers]:  # VCC needs tracks without its plane
            if len(points) > 1:
                nets[net] = points
        else:  # each surface pad on a plane has a via down to it
            pads += [via_box(*at[pin]) for pin in pins if pin in at and not shape(pin)[2]]
    for pin, (x, y) in at.items():
        w, h, through = shape(pin)
        pads.append((x - w / 2, y - h / 2, x + w / 2, y + h / 2, through))
    grid = rudy((layout.width, layout.height), nets, tile, pads, signal_layers)
    values = [u for row in grid for u in row]
    overflow = sum((u - BUSY) ** 2 for u in values if u > BUSY) * tile * tile
    peaks = {}
    for chip, slot in zip(chips, slots):
        x, y = (c + d for c, d in zip(placer.centre(slot), shift))
        w, h = layout.slot_of[chip]
        rows_under = range(max(0, int((y - h / 2) // tile)), min(len(grid), math.ceil((y + h / 2) / tile)))
        cols_under = range(max(0, int((x - w / 2) // tile)), min(len(grid[0]), math.ceil((x + w / 2) / tile)))
        under = sorted(grid[r][c] for r in rows_under for c in cols_under)
        peaks[chip] = under[int(0.9 * (len(under) - 1))] if under else 0.0
    return overflow, max(values), peaks


def inventory(chips, slot_of, density):
    """How many slots of each size: each size's chips and density - 1 spares (at least one)."""
    counts = {}
    for chip in chips:
        counts[slot_of[chip]] = counts.get(slot_of[chip], 0) + 1
    return {size: n + max(1, math.ceil(n * (density - 1))) if density > 1 else n for size, n in counts.items()}


def place_project(directory, gap=(7.0, 4.0), density=1.4, runs=6, moves=None, seed=1, cli=None, candidates=5,
                  template=None, packing='auto', vertical=False, label=None, back=False, cohesion=3.0, inflate=0,
                  signal_layers=2):
    """Writes Candidates/1..N.kicad_pcb and <project>.kicad_pcb (a copy of the best); returns that path. label is
    (text, corner, angle, size) for the card's name in silkscreen, or None. gap is
    (along each chip, across it), so it suits chips on their sides or, with vertical, standing up."""
    chip_angle = UPRIGHT if vertical else CHIP_ANGLE
    if vertical:
        gap = gap[::-1]
    name = os.path.basename(os.path.normpath(directory))
    netlist = os.path.join(directory, name + '.net')
    subprocess.run([cli or kicad_cli(), 'sch', 'export', 'netlist', '--format', 'kicadsexpr', '-o', netlist,
                    os.path.join(directory, name + '.kicad_sch')], check=True, capture_output=True)
    design = Design(netlist, find_kicad('share/kicad/footprints'))
    template = Template(template) if template else None
    alias = template.match(design) if template else {}
    on_template = set(alias)
    for ref, t in sorted(alias.items()):
        print('  %s is the template\'s %s' % (ref, t) if ref != t else '  %s is on the template' % ref)
    for x0, y0, x1, y1 in template.zones if template else ():
        print('  keepout %.0f x %.0f mm at (%.0f, %.0f)' % (x1 - x0, y1 - y0, x0, y0))
    kinds = {kind: sorted((r for r in design.parts if design.kind(r) == kind and r not in on_template),
                          key=reference_order)
             for kind in ('chip', 'decoupling', 'bulk', 'header')}
    chips = kinds['chip']
    if template and kinds['header']:
        print('  warning: %s matched nothing on the template, so it goes in the strip along the bottom edge'
              % ', '.join(kinds['header']))

    offsets, sizes, shapes = {}, {}, {}
    for reference in design.parts:
        angle = 90 if design.kind(reference) == 'header' else chip_angle
        offsets[reference], sizes[reference], _, shapes[reference] = pad_offsets(design.load(reference), angle)
    header_height = max((sizes[h][1] for h in kinds['header'] + kinds['bulk']), default=0)
    strip = header_height + EDGE  # the header and bulk cap row along the bottom edge
    signal_nets = [pins for net, pins in design.nets.items() if net.lstrip('/') not in POWER]
    groups = hierarchy_groups(design, chips)
    if cohesion:
        print('  %d blocks pull their chips together (cohesion %g)' % (len(groups), cohesion))
    moves = moves or 4000 * len(chips)

    def lay(slot_of, first=True):
        """Packs the chips' slots (slot_of: chip -> slot size) and anneals them: a Layout, or None when a later
        round's slots don't pack."""
        if template:
            # The board is fixed: the most spares that still pack, from density 2 down to none. Aligned rows unless
            # they don't fit or leave it tight, and free packing would give more room.
            width, height = template.size
            obstacles = [(x0 - 1, y0 - 1, x1 + 1, y1 + 1) for x0, y0, x1, y1 in template.keepouts]

            def densest(packer):
                for step in range(21):
                    density = round(2.0 - step * 0.05, 2)
                    counts = inventory(chips, slot_of, density)
                    result = packer([s for s, n in counts.items() for _ in range(n)], EDGE, EDGE, width - EDGE,
                                    height - strip, obstacles)
                    if result:
                        return density, result
                return 0, None

            used, packed = densest(rows) if packing != 'free' else (0, None)
            if packing != 'rows' and (packed is None or used < ROOMY):
                free_used, free_packed = densest(pack)
                if free_packed and free_used > used:
                    print('  rows %s, so the chips are packed freely' % ('leave density %.2f' % used if packed
                                                                           else "don't fit"))
                    used, packed = free_used, free_packed
            if packed is None:
                if not first:
                    return None
                need = sum(w * h for w, h in slot_of.values())
                raise SystemExit('%d chips need %.0f cm2 of slots and do not pack onto the %g x %g mm template (%.0f '
                                 'cm2 inside the edges); try a smaller --gap or a bigger board'
                                 % (len(chips), need / 100, width, height,
                                    (width - 2 * EDGE) * (height - EDGE - strip) / 100))
            print('  %d spare slots (density %.2f)' % (len(packed) - len(chips), used))
        else:
            obstacles = []
            counts = inventory(chips, slot_of, density)
            slots = [s for s, n in counts.items() for _ in range(n)]
            area = sum(w * h for w, h in slots)
            inner = max(math.sqrt(area * 1.6) * 1.1, max(w for w, _ in slots),
                        sum(sizes[h][0] + 5 for h in kinds['header'] + kinds['bulk']))
            packed = (pack if packing == 'free' else rows)(slots, EDGE, EDGE, EDGE + inner, float('inf'))
            width = 2 * EDGE + inner
            height = max(y + h / 2 for (_, h), (_, y) in packed) + EDGE + strip
        centres = [centre for _, centre in packed]
        slot_classes = [size for size, _ in packed]

        # Headers in a row along the bottom edge, then the bulk cap after them, stepping past template footprints.
        # The row starts from the side the template's connector is on, so the bulk cap sits by where power comes in.
        fixed_at, y = {}, height - EDGE - header_height / 2
        entry = [x for (r, _), (x, _) in template.pads.items() if r in alias.values()] if template else []
        step = -1 if entry and sum(entry) / len(entry) > width / 2 else 1
        x = EDGE if step > 0 else width - EDGE  # the row's leading edge

        for part in kinds['header'] + kinds['bulk']:
            w = sizes[part][0]
            while template and not template.clear(min(x, x + step * w), y - header_height / 2, max(x, x + step * w),
                                                  y + header_height / 2):
                x += step
            fixed_at[part] = (x + step * w / 2, y)
            x += step * (w + 5)
        if template and not EDGE <= x - step * 5 <= width - EDGE:
            print('  warning: the headers and bulk cap run past the board edge')
        fixed = {(ref, pad): (cx + dx, cy + dy) for ref, (cx, cy) in fixed_at.items()
                 for pad, (dx, dy) in offsets[ref].items()}
        if template:
            fixed.update({(ref, pad): at for ref, t in alias.items() for (r, pad), at in template.pads.items()
                          if r == t})

        placer = Placer(chips, offsets, signal_nets, fixed, centres, [slot_of[c] for c in chips], slot_classes,
                        groups, cohesion)
        rng = random.Random(seed)
        results = []
        start = time.time()
        for run in range(runs):
            cost, slots = placer.anneal(rng, moves)
            results.append((cost, slots))
            print('  anneal %d of %d: score %.0f, wirelength %.0f mm (%.0f s)'
                  % (run + 1, runs, cost, placer.wirelength(slots), time.time() - start))
        random_cost = sum(placer.wirelength(placer.anneal(random.Random(seed + run), 0)[1]) for run in range(5)) / 5
        results.sort()
        region = (EDGE, EDGE, width - EDGE, height - strip)
        results = [(cost, slots, centring([(placer.centre(s), slot_of[c]) for c, s in zip(chips, slots)], region,
                                          obstacles)) for cost, slots in results]
        print('%s: %d chips in %d slots (%s); best %.0f mm, random %.0f mm'
              % (name, len(chips), len(centres), ', '.join('%d of %g x %g' % (slot_classes.count(s), s[0], s[1])
                                                           for s in sorted(set(slot_classes))),
                 placer.wirelength(results[0][1]), random_cost))
        return Layout(width, height, fixed, fixed_at, placer, results, slot_of)

    # The inventory: each chip's slot size, from its own footprint plus its gap, which --inflate grows on the chips
    # in congestion's hot spots and shrinks, across the chip only (along it is its cap's room), on the quiet ones to
    # pay for it. Gaps change in steps, so chips alike mostly stay alike and still swap slots.
    across = 0 if vertical else 1
    grown = {chip: (1.0, 1.0) for chip in chips}  # each chip's gap factors, (x, y)

    def slot_sizes(factors):
        return {chip: slot_size(sizes[chip], tuple(g * f for g, f in zip(gap, factors[chip]))) for chip in chips}

    layout = lay(slot_sizes(grown))
    if inflate:
        score = congestion_of(design, chips, offsets, shapes, layout, signal_layers)
        print('  congestion: overflow %.0f, peak %.2f' % score[:2])
        for step in range(inflate):
            # Chips of one footprint and gap change together, by their median heat, so they keep sharing slots: on a
            # full board a chip whose slot is a size of its own can't move at all.
            classes = {}
            for chip in chips:
                classes.setdefault((sizes[chip], grown[chip]), []).append(chip)
            heat = {}
            for members in classes.values():
                peaks = sorted(score[2][c] for c in members)
                heat.update({c: peaks[len(peaks) // 2] for c in members})
            ranked = sorted(chips, key=lambda c: -heat[c])
            hot = [c for c in ranked if heat[c] > BUSY and max(grown[c]) < MOST_GROWN]
            cold = [c for c in ranked[::-1] if heat[c] < BUSY / 2 and grown[c][across] > LEAST_GROWN]
            if not hot:
                print('  inflate: no congested chip can grow')
                break
            attempt = None
            while hot and attempt is None:  # the hottest that still pack: all of them, else half, a quarter, ...
                trial = dict(grown)
                for chip in hot:
                    trial[chip] = tuple(min(MOST_GROWN, f + GROW_STEP) for f in grown[chip])
                for chip in cold:
                    trial[chip] = tuple(max(LEAST_GROWN, f - GROW_STEP) if i == across else f
                                        for i, f in enumerate(grown[chip]))
                print('== inflate %d of %d: %d hot chips grow (%s), %d quiet ones shrink'
                      % (step + 1, inflate, len(hot), ', '.join(sorted(hot, key=reference_order)), len(cold)),
                      flush=True)
                attempt = lay(slot_sizes(trial), first=False)
                if attempt is None:
                    print("  they don't pack")
                    hot = hot[:len(hot) // 2]
            if attempt is None:
                print('  keeping the placement before')
                break
            grown = trial
            new = congestion_of(design, chips, offsets, shapes, attempt, signal_layers)
            print('  congestion: overflow %.0f, peak %.2f (was %.0f, %.2f)' % (new[:2] + score[:2]))
            if new[0] >= score[0]:
                print('  no better: keeping the placement before')
                break
            layout, score = attempt, new
    width, height, fixed_at, placer, results, slot_of = (layout.width, layout.height, layout.fixed_at,
                                                         layout.placer, layout.results, layout.slot_of)

    def write(slots, shift, path):
        write_board(design, kinds, sizes, offsets, placer, fixed_at, slots, width, height, path, template, alias,
                    shift, chip_angle, label, back, signal_layers)
        print('  -> %s (%.0f x %.0f mm)' % (path, width, height))

    # The best few placements become candidates for route.py, which routes each and keeps the best.
    folder = os.path.join(directory, 'Candidates')
    os.makedirs(folder, exist_ok=True)
    for old in os.listdir(folder):  # KiCad leaves <candidate>-backups folders here when one is opened
        old = os.path.join(folder, old)
        shutil.rmtree(old) if os.path.isdir(old) else os.remove(old)
    for k, (_, slots, shift) in enumerate(results[:candidates]):
        write(slots, shift, os.path.join(folder, '%d.kicad_pcb' % (k + 1)))
    path = os.path.join(directory, name + '.kicad_pcb')
    write(*results[0][1:], path=path)
    if alias:
        write_pulls(design, alias, template, chips, placer, *results[0][1:], offsets,
                    os.path.join(directory, name + '.pulls.json'))
    return path


def renet(directory, cli=None):
    """Puts the schematic's nets back on the pads of the boards already placed, without moving anything: for after
    layout.py moves the card's connector pins, which only changes which net each connector pad has."""
    name = os.path.basename(os.path.normpath(directory))
    netlist = os.path.join(directory, name + '.net')
    subprocess.run([cli or kicad_cli(), 'sch', 'export', 'netlist', '--format', 'kicadsexpr', '-o', netlist,
                    os.path.join(directory, name + '.kicad_sch')], check=True, capture_output=True)
    design = Design(netlist, find_kicad('share/kicad/footprints'))
    pad_net = {(ref, pad): net for net, pins in design.nets.items() for ref, pad in pins}
    folder = os.path.join(directory, 'Candidates')
    boards = [os.path.join(directory, name + '.kicad_pcb')] + (sorted(
        os.path.join(folder, f) for f in os.listdir(folder) if f.endswith('.kicad_pcb')) if os.path.isdir(folder) else [])
    for path in boards:
        if not os.path.isfile(path):
            raise SystemExit('%s is not placed yet: run place.py without --renet first' % path)
        board = pcbnew.LoadBoard(path)
        changed = 0
        for footprint in board.GetFootprints():
            for pad in footprint.Pads():
                want = pad_net.get((footprint.GetReference(), pad.GetNumber()), '')
                if pad.GetNetname() == want:
                    continue
                item = board.FindNet(want) if want else None
                if want and item is None:
                    item = pcbnew.NETINFO_ITEM(board, want)
                    board.Add(item)
                if item is None:
                    pad.SetNetCode(0)
                else:
                    pad.SetNet(item)
                changed += 1
        board.BuildConnectivity()
        pcbnew.SaveBoard(path, board)
        print('  %s: %d pads on new nets' % (os.path.relpath(path, directory), changed))


def write_pulls(design, alias, template, chips, placer, slots, shift, offsets, path):
    """For layout.py: each connector pin's place, and for each net on the connector where the chips on
    it sit (their pads' centre), so the card's ports can be dealt to the pins nearest the chips that use them. In mm from the corner."""
    at = {}
    for chip, slot in zip(chips, slots):
        x, y = (c + d for c, d in zip(placer.centre(slot), shift))
        for pad, (dx, dy) in offsets[chip].items():
            at[(chip, pad)] = (x + dx, y + dy)
    net_of = {pin: net for net, pins in design.nets.items() for pin in pins}
    pins, pulls = {}, {}
    for ref, t in alias.items():
        for (r, pad), xy in template.pads.items():
            if r != t:
                continue
            pins['%s.%s' % (ref, pad)] = [round(v, 2) for v in xy]
            net = net_of.get((ref, pad))
            points = [at[p] for p in design.nets.get(net, []) if p in at]
            if net and net.lstrip('/') not in POWER and points:
                pulls[net] = [round(sum(p[i] for p in points) / len(points), 2) for i in (0, 1)]
    with open(path, 'w', encoding='utf-8') as file:
        json.dump({'pins': pins, 'pulls': pulls}, file, indent=1)
    print('  -> %s (%d connector pins pulled by chips)' % (path, len(pulls)))


def write_board(design, kinds, sizes, offsets, placer, fixed_at, best, width, height, path, template=None,
                alias=None, shift=(0, 0), chip_angle=CHIP_ANGLE, label=None, back=False, signal_layers=2):
    chips = kinds['chip']
    board = Board(design, width, height, template, alias)
    vcc = {}  # chip -> its VCC pad, in mm
    for chip, slot in zip(chips, best):
        centre = tuple(c + d for c, d in zip(placer.centre(slot), shift))
        footprint = board.place(chip, centre, chip_angle)
        value = design.parts[chip][0]
        lines = [value, design.blocks[chip]] + ([chip] if back else [])
        board.mark(footprint, centre, 90 if chip_angle == UPRIGHT else 0, lines, max(sizes[chip]),
                   back)
        # The VCC pad at the end of the chip: a '74 also ties preset and clear pins to VCC mid-row.
        pads = [offsets[chip][pad] for ref, pad in design.nets.get(net_name(design.nets, 'VCC'), []) if ref == chip]
        if pads:
            dx, dy = max(pads, key=(lambda o: (abs(o[1]), o[0])) if chip_angle == UPRIGHT
                         else (lambda o: (abs(o[0]), -o[1])))
            vcc[chip] = (centre[0] + dx, centre[1] + dy, centre)
    for reference, at in fixed_at.items():
        board.place(reference, at, 90 if design.kind(reference) == 'header' else chip_angle)
    # Caps are interchangeable: one beside each chip's VCC pad, just past the end of the chip (its right or left end
    # lying down, its top or bottom standing up).
    caps = kinds['decoupling']
    if len(caps) < len(vcc):
        print('  warning: %d chips but %d decoupling caps' % (len(vcc), len(caps)))
    for cap, (chip, (x, y, centre)) in zip(caps, sorted(vcc.items(), key=lambda item: reference_order(item[0]))):
        if chip_angle == UPRIGHT:
            outward = 1 if y >= centre[1] else -1
            board.place(cap, (x, y + outward * (CAP_GAP + sizes[cap][1] / 2)), chip_angle)
        else:
            outward = 1 if x >= centre[0] else -1
            board.place(cap, (x + outward * (CAP_GAP + sizes[cap][0] / 2), y), chip_angle)
    for k, cap in enumerate(caps[len(vcc):]):  # spares, in a row from the bottom right corner
        board.place(cap, (width - EDGE - (k + 0.5) * (sizes[cap][0] + CAP_GAP), height - EDGE), chip_angle)
    if label:
        board.label(*label)
    board.planes(signal_layers)
    board.save(path)


def reference_order(reference):
    letters = reference.rstrip('0123456789')
    return letters, int(reference[len(letters):] or 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('project', help='a project directory from kicad.py, e.g. "Build/KiCad/Register 16"')
    parser.add_argument('--gap', default='7x4', help="mm added to each chip's own size for its slot, AxB, along the chip by across it: room "
                        'for its cap and a routing channel (default 7x4)')
    parser.add_argument('--density', type=float, default=1.4, help='slots per chip (default 1.4)')
    parser.add_argument('--runs', type=int, default=6, help='independent anneals (default 6)')
    parser.add_argument('--candidates', type=int, default=5, help='best placements kept for routing (default 5)')
    parser.add_argument('--moves', type=int, help='moves per anneal (default 4000 per chip)')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--template', help='a board to place into, such as "Tools/eda/templates/6U_Eurocard/6U_Eurocard.kicad_pcb". '
                        'Its outline and footprints stay; --density is ignored')
    parser.add_argument('--packing', choices=('auto', 'rows', 'free'), default='auto', help='aligned rows, or free '
                        "packing that stacks short chips beside tall ones; auto packs freely only when rows don't fit "
                        'or leave fewer than %g slots per chip (default auto)' % ROOMY)
    parser.add_argument('--vertical', action='store_true', help='stand the chips up, pin 1 top left, so rows of '
                        'them read as columns running toward the connectors (default: on their sides)')
    parser.add_argument('--label', help="the card's name in silkscreen (default: the project's name; 'none' for no "
                        'label)')
    parser.add_argument('--label-corner', choices=('auto', 'top-left', 'top-right', 'bottom-left', 'bottom-right'),
                        default='auto', help='where the label goes if that corner is clear of parts (default auto: '
                        'the first clear corner)')
    parser.add_argument('--label-angle', type=float, default=0, help='label rotation in degrees (default 0)')
    parser.add_argument('--label-size', type=float, default=3.0, help='label text height in mm (default 3)')
    parser.add_argument('--cohesion', type=float, default=3.0, help="how strongly each block's chips (a register, "
                        'its ports) are pulled together; 0 turns it off (default 3)')
    parser.add_argument('--inflate', type=int, default=0, help="rounds that grow the congested chips' slots and "
                        'place again (default 0: none)')
    parser.add_argument('--signal-layers', type=int, choices=(2, 3), default=2, help='2: In1 a GND plane and In2 '
                        'a VCC plane; 3: In2 a third signal layer too, VCC routed as tracks, which route.py tops up '
                        'with a VCC pour on In2 (default 2)')
    parser.add_argument('--back-labels', action='store_true', help="each chip's part number, block and reference "
                        'mirrored on the back silkscreen too')
    parser.add_argument('--kicad-cli', help="exports the schematic's netlist (default: KiCad's own)")
    parser.add_argument('--renet', action='store_true', help="don't place: put the schematic's nets on the boards "
                        'already placed, after layout.py moves the connector pins')
    args = parser.parse_args()
    if args.renet:
        renet(args.project, args.kicad_cli)
        return
    text = args.label or os.path.basename(os.path.normpath(args.project))
    label = None if text.lower() == 'none' else (text, args.label_corner, args.label_angle, args.label_size)
    gap = tuple(float(v) for v in args.gap.lower().split('x'))
    place_project(args.project, gap, args.density, args.runs, args.moves, args.seed, args.kicad_cli,
                  args.candidates, args.template, args.packing, args.vertical,
                  label, args.back_labels, args.cohesion, args.inflate, args.signal_layers)


if __name__ == '__main__':
    main()
