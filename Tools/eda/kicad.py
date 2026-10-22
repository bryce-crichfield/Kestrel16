"""Turns a component netlist into a KiCad project: one schematic sheet per component, chips from parts.yaml.

    python Tools/eda/kicad.py "Kestrel16 Computer" [-o DIR] [--check]

Each component becomes a .kicad_sch file. A component used inside another becomes a hierarchical sheet,
and one file serves every instance, so four Register 16s share one sheet with four sets of references.
The top component is a card: its ports become global labels, and its backplane connectors, P1 and P2, are drawn with
the card template's footprint. floorplan.py's pin map says which port goes on which connector pin, and which pins
are GND and VCC; without one, the pins are left unconnected.

Nothing is wired with lines. Every pin gets a label naming its net, and pins with the same label are
connected. A net is named after its port if it has one, GND or VCC if it is tied there (power symbols),
or after the pin that drives it. Gates of one kind in a sheet are packed into packages unit by unit.
Leftover units are tied off below the drawing, next to each package's power unit. A child component with
an entry in parts.yaml (Decoder 3to8, Register 16, ...) gets no sheet: its real chips go below the drawing.

--check proves the schematic matches the netlist. kicad-cli exports the schematic's netlist, which is
mapped back through the references and pin numbers recorded here and compared net by net with the
flattened source netlist. It also runs KiCad's electrical rules check.
"""
import argparse
import copy
import json
import os
import subprocess
import sys
import uuid

import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kicad_env import find_kicad, kicad_cli, net_settings  # noqa: E402
from netlist import Library, bits  # noqa: E402
import sexpr  # noqa: E402
from sexpr import Sym, find, find_all  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SCALE = 0.2         # millimetres per simulator pixel
GRID = 2.54
MARGIN = 25.4
SLOT = 40.64        # the grid cell for tie-offs and power units below the drawing
LANE_PITCH = 66.04  # how far apart the chips of a memory wider than one chip sit
FONT = [Sym('effects'), [Sym('font'), [Sym('size'), 1.27, 1.27]]]
HIDDEN = [Sym('effects'), [Sym('font'), [Sym('size'), 1.27, 1.27]], [Sym('hide'), Sym('yes')]]
NAMESPACE = uuid.UUID('9b8a3c52-27f4-4a8e-9a43-6f1e0c3f5d10')
POWER_KINDS = {'Low': 'GND', 'High': 'VCC'}
# Every top component is a card, and every card plugs into the backplane through this, as on the card template.
CONNECTOR = ('Connector:DIN41612_03x32_C_Split', 'Connector_DIN:DIN41612_C_3x32_Male_Horizontal_THT')
CONNECTORS = ('P1', 'P2')  # two 96-pin connectors along the card's long edge
# ERC findings the unmapped connector causes: the card's inputs drive nothing. Unwired inputs inside are tied to GND,
# so an undriven pin can only be on a card input's net.
UNMAPPED = {'pin_not_driven', 'label_dangling', 'global_label_dangling'}


def pins_path(component):
    """Where layout.py leaves a card's own pins, dealt by its placement."""
    return os.path.join(HERE, '..', '..', 'Build', 'Cards', component + ' Pins.yaml')


def load_floorplan(component, path=None, straight=False):
    """(port -> pin like P1.b7, pin -> GND or VCC) for a card from floorplan.py's map, or None if the component is
    on none. Without a path, the maps in Build/Cards are searched for the card (the component's name less "Card ").
    The card's own pins from layout.py win over the map's, unless straight, or they no longer cover the same ports."""
    found = find_floorplan(component, path)
    if found is None or straight or not os.path.isfile(pins_path(component)):
        return found
    with open(pins_path(component), encoding='utf-8') as file:
        own = yaml.safe_load(file) or {}
    if set(own) != set(found[0]):
        print('  %s is for other ports than the floorplan has now, so it is ignored: rerun layout.py'
              % os.path.relpath(pins_path(component)))
        return found
    print('  pins from %s, dealt by placement' % os.path.relpath(pins_path(component)))
    return own, found[1]


def find_floorplan(component, path=None):
    card = component[len('Card '):] if component.startswith('Card ') else component
    folder = os.path.join(HERE, '..', '..', 'Build', 'Cards')
    paths = [path] if path else sorted(os.path.join(folder, name) for name in os.listdir(folder)
                                       if name.endswith(' Floorplan.yaml')) if os.path.isdir(folder) else []
    for candidate in paths:
        with open(candidate, encoding='utf-8') as file:
            plan = yaml.safe_load(file)
        if card in plan['cards']:
            power = {pin: net for net, pins in plan['power'].items() for pin in pins}
            return plan['cards'][card], power
    if path:
        raise SystemExit('%s has no card %s' % (path, card))
    return None


def sheet_name(node, siblings):
    """A child sheet's name: the instance's own name from hdl (R3, IR), else its component's, then " #<node id>"
    unless the name alone is unique among its siblings. KiCad wants names unique within a sheet, and place.py
    drops the " #<id>" when it labels the chips."""
    name = node.name or node.component
    if node.name and sum(1 for other, *_ in siblings if other.name == node.name) == 1:
        return name
    return '%s #%d' % (name, node.id)


def kicad_name(name):
    """/LOAD reads as an overbar in KiCad: ~{LOAD}."""
    return '~{%s}' % name[1:] if name.startswith('/') else name


def snap(value):
    return round(value / GRID) * GRID


# ---------------------------------------------------------------------------
# KiCad's symbol libraries

class Symbol:
    """A library symbol: its definition for lib_symbols and where its pins are."""

    def __init__(self, lib_id, expr):
        self.lib_id = lib_id
        self.expr = expr
        self.pins = {}  # number -> (unit, x, y, angle, electrical type)
        self.units = 1
        name = lib_id.split(':')[1]
        for unit_expr in find_all(expr, 'symbol'):
            unit, style = (int(part) for part in unit_expr[1][len(name) + 1:].split('_'))
            self.units = max(self.units, unit)
            if style > 1:
                continue  # the De Morgan drawing
            for pin in find_all(unit_expr, 'pin'):
                at = find(pin, 'at')
                self.pins[str(find(pin, 'number')[1])] = (unit, float(at[1]), float(at[2]), int(float(at[3])), str(pin[1]))
        properties = {prop[1]: prop[2] for prop in find_all(expr, 'property')}
        self.prefix = properties.get('Reference', 'U')

    def unit_of(self, numbers):
        units = {self.pins[str(number)][0] for number in numbers} - {0}
        if len(units) > 1:
            raise ValueError('%s: pins %s are on different units' % (self.lib_id, numbers))
        return units.pop() if units else 1

    def unit_pins(self, unit):
        return [number for number, pin in self.pins.items() if pin[0] in (0, unit)]


class Symbols:
    def __init__(self, directory):
        self.directory = directory
        self.libraries = {}
        self.symbols = {}

    def _library(self, name):
        if name not in self.libraries:
            path = os.path.join(self.directory, name + '.kicad_sym')
            with open(path, encoding='utf-8') as file:
                self.libraries[name] = {item[1]: item for item in find_all(sexpr.parse(file.read()), 'symbol')}
        return self.libraries[name]

    def _resolve(self, library, name):
        expr = self._library(library)[name]
        extends = find(expr, 'extends')
        if not extends:
            return copy.deepcopy(expr)
        base_name = extends[1]
        merged = self._resolve(library, base_name)
        merged[1] = name
        for unit in find_all(merged, 'symbol'):
            unit[1] = name + unit[1][len(base_name):]
        overrides = {prop[1]: prop for prop in find_all(expr, 'property')}
        for index, item in enumerate(merged):
            if isinstance(item, list) and item[:1] == ['property'] and item[1] in overrides:
                merged[index] = overrides.pop(item[1])
        return merged

    def get(self, lib_id):
        if lib_id not in self.symbols:
            library, name = lib_id.split(':')
            expr = self._resolve(library, name)
            expr[1] = lib_id
            self.symbols[lib_id] = Symbol(lib_id, expr)
        return self.symbols[lib_id]


# ---------------------------------------------------------------------------
# One sheet per component

class Placed:
    """A symbol unit on a sheet. package is the key it shares with its package's other units."""

    def __init__(self, symbol, unit, at, package, value, footprint='', logical=None):
        self.symbol, self.unit, self.at, self.package = symbol, unit, at, package
        self.value, self.footprint = str(value), str(footprint)
        self.logical = logical or {}  # hardware pin -> (node id, logical pin)
        self.fields = {}              # more properties, like a memory's Image
        self.uuid = None
        self.reference = None         # a fixed reference, like the connector's P1; otherwise annotated

    def pin_point(self, number):
        _, x, y, angle, _ = self.symbol.pins[str(number)]
        return (round(self.at[0] + x, 4), round(self.at[1] - y, 4)), angle


class Sheet:
    def __init__(self, project, component, top):
        self.project, self.component, self.top = project, component, top
        self.items = []
        self.placed = []      # Placed symbols, logic and power alike
        self.sheets = []      # (node, uuid, at, size, pins)
        self.packages = []    # package keys in reference order
        self.cells = None     # free cells below the drawing: (top, columns, [next index])
        self.bottom = 0
        self.right = 0
        self.build()

    # -- helpers

    def uuid(self, key):
        return str(uuid.uuid5(NAMESPACE, '%s|%s' % (self.component.name, key)))

    def item(self, expr):
        self.items.append(expr)

    def take_cell(self):
        """The next free cell in rows below everything placed before the first call."""
        if self.cells is None:
            self.cells = (self.bottom, max(8, int((self.right - MARGIN) // SLOT)), [0])
        top, columns, used = self.cells
        index = used[0]
        used[0] += 1
        return snap(MARGIN + (index % columns + 0.5) * SLOT), snap(top + SLOT * (index // columns + 1))

    def extend(self, x, y):
        self.right = max(self.right, x)
        self.bottom = max(self.bottom, y)

    def place(self, placed):
        placed.uuid = self.uuid('symbol%d' % len(self.placed))
        self.placed.append(placed)
        if placed.package not in self.packages:
            self.packages.append(placed.package)
        for number in placed.symbol.unit_pins(placed.unit):
            point, _ = placed.pin_point(number)
            self.extend(*point)
        return placed

    # -- connections

    def attach(self, name, point, angle):
        """A label, or power symbol, naming net name at point. angle is the pin's, so the label points away."""
        kind, text = name[:2]
        x, y = point
        label_angle = (angle + 180) % 360
        justify = [Sym('justify'), Sym('right' if label_angle in (180, 270) else 'left')]
        if kind == 'power':
            symbol = self.project.symbols.get('power:' + text)
            self.place(Placed(symbol, 1, point, ('#PWR', len(self.placed)), text))
        elif kind == 'local':
            self.item([Sym('label'), text, [Sym('at'), x, y, label_angle], FONT + [justify], [Sym('uuid'), self.uuid(('label', point))]])
        else:
            shape = [Sym('shape'), Sym(name[2] if len(name) > 2 else 'passive')]
            head = Sym('hierarchical_label' if kind == 'hier' else 'global_label')
            expr = [head, text, shape, [Sym('at'), x, y, label_angle], FONT + [justify], [Sym('uuid'), self.uuid(('label', point))]]
            if kind == 'global':
                expr.append([Sym('property'), 'Intersheetrefs', '${INTERSHEET_REFS}', [Sym('at'), x, y, 0], HIDDEN])
            self.item(expr)

    def no_connect(self, point):
        self.item([Sym('no_connect'), [Sym('at'), point[0], point[1]], [Sym('uuid'), self.uuid(('nc', point))]])

    def wire(self, a, b):
        self.item([Sym('wire'), [Sym('pts'), [Sym('xy'), a[0], a[1]], [Sym('xy'), b[0], b[1]]],
                   [Sym('stroke'), [Sym('width'), 0], [Sym('type'), Sym('default')]], [Sym('uuid'), self.uuid(('wire', a, b))]])

    def join(self, first, second):
        """Two names for one net, joined by a short wire in a free cell."""
        x, y = self.take_cell()
        self.attach(first, (x - 5.08, y), 0)
        self.wire((x - 5.08, y), (x + 5.08, y))
        self.attach(second, (x + 5.08, y), 180)

    # -- the sheet

    def build(self):
        parts, symbols = self.project.parts, self.project.symbols
        nodes = self.component.nodes
        to_mm = lambda at: (snap(MARGIN + at[0] * SCALE), snap(MARGIN + at[1] * SCALE))
        endpoints = {}   # (node id, pin) -> [(point, pin angle)], several when a node takes several chips
        inputs = set()   # pins that read their net, for tying off when unconnected
        outputs = set()

        # Chips, packed. Each kind's nodes fill packages unit by unit, in node order.
        by_kind = {}
        for node in nodes.values():
            if node.kind in parts:
                by_kind.setdefault(node.kind, []).append(node)
            elif node.kind not in ('Input', 'Output', 'Component') and node.kind not in POWER_KINDS:
                raise ValueError('%s: no part in parts.yaml for %s' % (self.component.name, node.kind))
        self.leftovers = []
        self.package_parts = {}  # package -> its chip's entry, for chips of components parts.yaml maps
        for kind, kind_nodes in by_kind.items():
            part = parts[kind]
            symbol = symbols.get(part['symbol'])
            slots = max((len(pins) for pins in part.get('pins', {}).values() if isinstance(pins, list)), default=1)
            count = 0  # units used so far
            for node in kind_nodes:
                for lane, hardware, fields in self.hardware_pins(part, node, count % slots):
                    package, slot = (kind, count // slots), count % slots
                    count += 1
                    unit = symbol.unit_of(hardware.values())
                    at = to_mm(node.at)
                    placed = self.place(Placed(symbol, unit, (at[0], at[1] + lane * LANE_PITCH), package, part['value'],
                                               part.get('footprint', ''), {str(n): (node.id, pin) for pin, n in hardware.items()}))
                    placed.fields = fields
                    for pin, number in hardware.items():
                        endpoints.setdefault((node.id, pin), []).append(placed.pin_point(number))
                    inputs.update((node.id, pin) for pin in node.inputs if pin in hardware)
                    outputs.update((node.id, pin) for pin in node.outputs if pin in hardware)
                    self.tie_fixed(part, placed, slot, node)
                    if 'address' in part:
                        self.tie_memory(part, placed, node, len(hardware))
            for slot in range(count % slots and slots - count % slots):
                self.leftovers.append((kind, part, symbol, (kind, count // slots), count % slots + slot))

        # Child components, as sheets: inputs down the left edge, outputs down the right. Those parts.yaml maps
        # to real chips are left out here and placed below, as those chips.
        for node in self.component.children():
            if node.component in parts:
                continue
            at = to_mm(node.at)
            rows = max(len(node.inputs), len(node.outputs), 1)
            size = (30.48, (rows + 1) * GRID)
            sheet_uuid = self.uuid('sheet%d' % node.id)
            pins = []
            for side, names in (('input', node.inputs), ('output', node.outputs)):
                for row, pin in enumerate(names):
                    point = (at[0] + (size[0] if side == 'output' else 0), at[1] + (row + 1) * GRID)
                    pins.append((pin, side, point))
                    endpoints[(node.id, pin)] = [(point, 180 if side == 'output' else 0)]
                    (outputs if side == 'output' else inputs).add((node.id, pin))
            self.sheets.append((node, sheet_uuid, at, size, pins))
            self.extend(at[0] + size[0], at[1] + size[1])

        # Below the drawing, one chip per cell: the chips of components parts.yaml maps, in place of their sheets.
        for node in self.component.children():
            if node.component in parts:
                self.mapped(node, parts[node.component], endpoints, inputs, outputs)

        # Nets. Port nets carry the port's name; tied nets are GND or VCC; the rest are named after a driver.
        port_kind = 'global' if self.top else 'hier'  # the top's ports go to the backplane connector, once mapped
        seen = set()
        for net in self.component.nets:
            names, points = [], []
            for node_id, pin in net:
                node = nodes[node_id]
                if node.kind in POWER_KINDS:
                    names.insert(0, ('power', POWER_KINDS[node.kind]))
                elif node.kind in ('Input', 'Output'):
                    names.append((port_kind, kicad_name(pin), 'input' if node.kind == 'Input' else 'output'))
                elif (node_id, pin) in endpoints:
                    points += endpoints[(node_id, pin)]
                seen.add((node_id, pin))
            names = list(dict.fromkeys(names))
            if not names:
                driver = next(((n, p) for n, p in net if (n, p) in outputs), net[0])
                names = [('local', 'n%d_%s' % (driver[0], driver[1].replace('/', 'n').replace("'", '_out')))]
            for point, angle in points:
                self.attach(names[0], point, angle)
            for extra in names[1:]:
                self.join(names[0], extra)
            if not points and len(names) == 1 and names[0][0] in ('hier', 'global'):
                x, y = self.take_cell()  # a port nothing inside uses still needs its label
                self.attach(names[0], (x, y), 0)
                self.no_connect((x, y))
        for key, places in sorted(endpoints.items(), key=lambda item: (item[0][0], str(item[0][1]))):
            for point, angle in places if key not in seen else ():
                if key in inputs:
                    self.attach(('power', 'GND'), point, angle)  # the simulator reads an unconnected input as 0
                else:
                    self.no_connect(point)
        for node in self.component.ports('Input') + self.component.ports('Output'):
            for pin in bits(node.label, node.width):
                if not self.top and (node.id, pin) not in seen:
                    x, y = self.take_cell()
                    self.attach((port_kind, kicad_name(pin), node.kind.lower()), (x, y), 0)
                    self.no_connect((x, y))

        if self.top:
            self.connector()

        # Below the drawing: leftover units, tied off, then each package's power unit.
        for kind, part, symbol, package, slot in self.leftovers:
            hardware = {pin: numbers[slot] for pin, numbers in part['pins'].items()}
            unit = symbol.unit_of(hardware.values())
            placed = self.place(Placed(symbol, unit, self.take_cell(), package, part['value'], part.get('footprint', '')))
            for pin, number in hardware.items():
                point, angle = placed.pin_point(number)
                tie = part.get('unused', {}).get(pin)
                if tie:
                    self.attach(('power', tie), point, angle)
                else:
                    self.no_connect(point)
            self.tie_fixed(part, placed, slot, None)
        for package in list(self.packages):
            units = [placed for placed in self.placed if placed.package == package]
            first = units[0]
            part = self.package_parts.get(package) or parts.get(package[0])
            if not part:
                continue
            power = part.get('power', {})
            power_unit = first.symbol.unit_of(power.values()) if power else None
            holder = next((placed for placed in units if placed.unit == power_unit), None)
            if power and not holder:
                holder = self.place(Placed(first.symbol, power_unit, self.take_cell(), package, first.value, first.footprint))
            for net, number in power.items():
                self.attach(('power', net), *holder.pin_point(number))
            if power:  # its decoupling cap; the placer puts one beside each chip's VCC pin
                self.capacitor(('C', package), 'Device:C', '100nF', 'Capacitor_THT:C_Disc_D3.0mm_W1.6mm_P2.50mm')

        if self.top:  # one bulk cap where power comes in, and tell ERC that something supplies VCC and GND
            self.capacitor(('C', 'bulk'), 'Device:C_Polarized', '10uF',
                           'Capacitor_THT:CP_Radial_D5.0mm_P2.00mm')
            for net in ('VCC', 'GND'):
                point = self.take_cell()
                self.place(Placed(self.project.symbols.get('power:PWR_FLAG'), 1, point, ('#FLG', net), 'PWR_FLAG'))
                self.attach(('power', net), point, 90)

    def capacitor(self, package, lib_id, value, footprint):
        """A cap from VCC (pin 1) to GND (pin 2), below the drawing."""
        placed = self.place(Placed(self.project.symbols.get(lib_id), 1, self.take_cell(), package, value, footprint))
        self.attach(('power', 'VCC'), *placed.pin_point(1))
        self.attach(('power', 'GND'), *placed.pin_point(2))

    def connector(self):
        """The card's backplane connectors, P1 and P2 as on VME, right of the drawing: one unit per row. Their
        footprint matches the ones on the card template, so place.py hands the template's P1 and P2 these pads'
        nets. The floorplan's pin map puts each port's global label on its pin and power on the power pins; the
        rest, and every pin without a map, are left unconnected."""
        ports, power = self.project.floorplan or ({}, {})
        directions = {pin: node.kind.lower() for node in self.component.ports('Input') + self.component.ports('Output')
                      for pin in bits(node.label, node.width)}
        if self.project.floorplan:
            missing = sorted(set(directions) - set(ports))
            if missing:
                raise SystemExit('the floorplan has no pin for %s on %s: rerun floorplan.py after cards.py'
                                 % (', '.join(missing[:5]), self.component.name))
        on_pin = {pin: port for port, pin in ports.items() if port in directions}
        symbol = self.project.symbols.get(CONNECTOR[0])
        x = snap(self.right + SLOT)
        for k, (reference, unit) in enumerate((r, u) for r in CONNECTORS for u in range(1, symbol.units + 1)):
            placed = Placed(symbol, unit, (x + k * SLOT, MARGIN), ('P', reference), 'DIN 41612 96-pin',
                            CONNECTOR[1])
            placed.reference = reference
            low = min(-y for u, _, y, _, _ in symbol.pins.values() if u == unit)
            placed.at = (placed.at[0], snap(MARGIN - low))  # its top pin on the margin
            for number in symbol.unit_pins(unit):
                pin = '%s.%s' % (reference, number)
                if pin in on_pin:
                    placed.logical[number] = ('port', on_pin[pin])
            self.place(placed)
            for number in symbol.unit_pins(unit):
                pin = '%s.%s' % (reference, number)
                point, angle = placed.pin_point(number)
                if pin in power:
                    self.attach(('power', power[pin]), point, angle)
                elif pin in on_pin:
                    port = on_pin[pin]
                    self.attach(('global', kicad_name(port), directions[port]), point, angle)
                else:
                    self.no_connect(point)

    def chip_cell(self, symbol):
        """Where the next mapped chip goes: in rows below the drawing, each row as tall as its tallest chip, so
        no two chips' pins touch. Called before anything takes a cell, which then start below these rows."""
        ys = [-pin[2] for pin in symbol.pins.values()]
        xs = [pin[1] for pin in symbol.pins.values()]
        width, top, bottom = snap(max(xs) - min(xs) + 2 * SLOT / 2), min(ys), max(ys)
        if not hasattr(self, 'row'):
            self.row = [MARGIN, snap(self.bottom + SLOT), 0]  # x, y of the row's top, its height
        x, y, height = self.row
        if x + width > max(self.right, MARGIN + 8 * SLOT):
            x, y, height = MARGIN, snap(y + height + GRID * 4), 0
        at = (snap(x + width / 2), snap(y - top))
        self.row = [x + width, y, max(height, bottom - top)]
        return at

    def mapped(self, node, entry, endpoints, inputs, outputs):
        """A component placed as the chips its parts.yaml entry lists. Each chip's pins name either one of the
        component's port bits or, starting with '.', a net between its chips, like a register's stored bits."""
        internal = {}  # '.name' -> [(point, angle)]
        wired = set()
        for index, chip in enumerate(entry['chips']):
            symbol = self.project.symbols.get(chip['symbol'])
            package = (node.component, node.id, index)
            self.package_parts[package] = chip
            by_unit = {}
            for pin, numbers in chip['pins'].items():
                for number in numbers if isinstance(numbers, list) else [numbers]:
                    by_unit.setdefault(symbol.pins[str(number)][0] or 1, []).append((pin, number))
            for unit, pins in sorted(by_unit.items()):
                logical = {str(number): (node.id, pin) for pin, number in pins if not pin.startswith('.')}
                placed = self.place(Placed(symbol, unit, self.chip_cell(symbol), package, chip['value'],
                                           chip.get('footprint', ''), logical))
                if node.name:  # a mapped part has no sheet to carry its instance's name, so its chips do
                    placed.fields = dict(placed.fields, Block=node.name)
                for pin, number in pins:
                    if pin.startswith('.'):
                        internal.setdefault(pin, []).append(placed.pin_point(number))
                    else:
                        endpoints.setdefault((node.id, pin), []).append(placed.pin_point(number))
                        wired.add(pin)
                self.tie_fixed(chip, placed, 0, node)
        missing = [pin for pin in node.pins() if pin not in wired]
        if missing:
            raise ValueError('%s: parts.yaml gives %s no pin for %s' % (self.component.name, node.component,
                                                                        ', '.join(missing)))
        inputs.update((node.id, pin) for pin in node.inputs)
        outputs.update((node.id, pin) for pin in node.outputs)
        for name, points in internal.items():
            for point, angle in points:
                self.attach(('local', 'n%d_%s' % (node.id, name[1:])), point, angle)

    def hardware_pins(self, part, node, slot):
        """(lane, {logical pin: hardware pin number}, extra fields) for each chip one node takes.
        A memory wider than the chip takes several, side by side on the same address lines."""
        common = {pin: numbers[slot] if isinstance(numbers, list) else numbers
                  for pin, numbers in part.get('pins', {}).items()}
        if 'address' not in part:
            return [(0, common, {})]
        if len(node.address) > len(part['address']):
            raise ValueError('%s: node %d needs %d address pins; a %s has %d'
                             % (self.component.name, node.id, len(node.address), part['value'], len(part['address'])))
        width = len(part['data'])
        lanes = []
        for lane, first in enumerate(range(0, len(node.data), width)):
            data = node.data[first:first + width]
            hardware = dict(zip(node.address, part['address']), **common)
            hardware.update(zip(data, part['data']))
            image = node.entry.get('image', '')
            if node.entry.get('lane'):
                image += ' (%s bytes)' % node.entry['lane']
            if len(node.data) > width:
                image += ' (data bits %d-%d)' % (first, first + len(data) - 1)
            lanes.append((lane, hardware, {'Image': image} if image else {}))
        return lanes

    def tie_fixed(self, part, placed, slot, node):
        for net, numbers in part.get('tie', {}).items():
            for number in numbers:
                if placed.symbol.pins[str(number)][0] in (0, placed.unit):
                    self.attach(('power', net), *placed.pin_point(number))

    def tie_memory(self, part, placed, node, used):
        """Address pins past the netlist's go to GND; spare data pins get no-connect flags."""
        for number in part['address'][len(node.address):]:
            self.attach(('power', 'GND'), *placed.pin_point(number))
        data = used - len(node.address) - len(part.get('pins', {}))
        for number in part['data'][data:]:
            self.no_connect(placed.pin_point(number)[0])


# ---------------------------------------------------------------------------
# The project: every sheet, annotated per instance, written out

class Project:
    def __init__(self, top, library, parts, symbols, floorplan=None):
        self.name = top
        self.library, self.parts, self.symbols = library, parts, symbols
        self.floorplan = floorplan  # (port -> pin, pin -> power net) for the top card's connectors, or None
        self.sheets = {}
        self.instances = {}  # component -> [(path string, node-id path)]
        self.root_uuid = str(uuid.uuid5(NAMESPACE, 'root|' + top))
        self._visit(top, '/' + self.root_uuid, ())
        self.pages = {}
        self._annotate()

    def _visit(self, name, path, ids):
        if name not in self.sheets:
            self.sheets[name] = Sheet(self, self.library.load(name), top=not ids)
        self.instances.setdefault(name, []).append((path, ids))
        for node, sheet_uuid, _, _, _ in self.sheets[name].sheets:
            self._visit(node.component, path + '/' + sheet_uuid, ids + (node.id,))

    def _annotate(self):
        counters = {}
        self.references = {}  # (component, path, package) -> reference
        self.pin_map = {}     # 'U12:3' -> [node-id path, node id, logical pin]
        page = 1
        order = []
        stack = [(self.name, '/' + self.root_uuid, ())]
        while stack:  # depth first, in node order, so references read top to bottom
            name, path, ids = stack.pop(0)
            order.append((name, path, ids))
            self.pages[path] = str(page)
            page += 1
            children = [(node.component, path + '/' + sheet_uuid, ids + (node.id,))
                        for node, sheet_uuid, _, _, _ in self.sheets[name].sheets]
            stack[:0] = children
        for name, path, ids in order:
            sheet = self.sheets[name]
            for package in sheet.packages:
                fixed = next((p.reference for p in sheet.placed if p.package == package and p.reference), None)
                if fixed:
                    self.references[(name, path, package)] = fixed
                    continue
                prefix = package[0] if package[0].startswith('#') else \
                    next(p.symbol.prefix for p in sheet.placed if p.package == package)
                counters[prefix] = counters.get(prefix, 0) + 1
                reference = '%s%d' % (prefix, counters[prefix]) if not prefix.startswith('#') else \
                    '%s%02d' % (prefix, counters[prefix])
                self.references[(name, path, package)] = reference
            for placed in sheet.placed:
                reference = self.references[(name, path, placed.package)]
                for number, (node_id, pin) in placed.logical.items():
                    self.pin_map['%s:%s' % (reference, number)] = \
                        ['port', pin] if node_id == 'port' else [list(ids), node_id, pin]

    def write(self, directory):
        os.makedirs(directory, exist_ok=True)
        for name, sheet in self.sheets.items():
            with open(os.path.join(directory, name + '.kicad_sch'), 'w', encoding='utf-8', newline='\n') as file:
                file.write(sexpr.write(self._schematic(name, sheet)) + '\n')
        with open(os.path.join(directory, self.name + '.kicad_pro'), 'w', encoding='utf-8', newline='\n') as file:
            json.dump({'meta': {'filename': self.name + '.kicad_pro', 'version': 1}, 'net_settings': net_settings()},
                      file, indent=2)
        with open(os.path.join(directory, self.name + '.pins.json'), 'w', encoding='utf-8', newline='\n') as file:
            json.dump(self.pin_map, file, indent=0, sort_keys=True)

    def _schematic(self, name, sheet):
        top = name == self.name
        lib_ids = sorted({placed.symbol.lib_id for placed in sheet.placed})
        width = max(297, snap(sheet.right + MARGIN + SLOT))
        height = max(210, snap(sheet.bottom + MARGIN + SLOT * ((sheet.cells[2][0] // sheet.cells[1] + 2) if sheet.cells else 1)))
        expr = [Sym('kicad_sch'), [Sym('version'), Sym('20250114')], [Sym('generator'), 'kestrel_kicad'],
                [Sym('generator_version'), '9.0'],
                [Sym('uuid'), self.root_uuid if top else sheet.uuid('sheet')],
                [Sym('paper'), 'User', width, height],
                [Sym('title_block'), [Sym('title'), name], [Sym('comment'), 1, 'Generated from Components/%s.yaml by Tools/eda/kicad.py' % name]],
                [Sym('lib_symbols')] + [self.symbols.get(lib_id).expr for lib_id in lib_ids]]
        instances = self.instances[name]
        for placed in sheet.placed:
            references = [self.references[(name, path, placed.package)] for path, _ in instances]
            symbol = [Sym('symbol'), [Sym('lib_id'), placed.symbol.lib_id], [Sym('at'), placed.at[0], placed.at[1], 0],
                      [Sym('unit'), placed.unit], [Sym('exclude_from_sim'), Sym('no')], [Sym('in_bom'), Sym('yes')],
                      [Sym('on_board'), Sym('yes')], [Sym('dnp'), Sym('no')], [Sym('uuid'), placed.uuid]]
            hide = placed.package[0].startswith('#')
            symbol += [[Sym('property'), 'Reference', references[0], [Sym('at'), placed.at[0], placed.at[1] - 5.08, 0], HIDDEN if hide else FONT],
                       [Sym('property'), 'Value', placed.value, [Sym('at'), placed.at[0], placed.at[1] + 5.08, 0], FONT],
                       [Sym('property'), 'Footprint', placed.footprint, [Sym('at'), placed.at[0], placed.at[1], 0], HIDDEN],
                       [Sym('property'), 'Datasheet', '', [Sym('at'), placed.at[0], placed.at[1], 0], HIDDEN]]
            symbol += [[Sym('property'), key, value, [Sym('at'), placed.at[0], placed.at[1] + 7.62, 0], FONT]
                       for key, value in placed.fields.items()]
            symbol += [[Sym('pin'), number, [Sym('uuid'), sheet.uuid((placed.uuid, number))]]
                       for number in placed.symbol.unit_pins(placed.unit)]
            symbol.append([Sym('instances'), [Sym('project'), self.name] +
                           [[Sym('path'), path, [Sym('reference'), reference], [Sym('unit'), placed.unit]]
                            for (path, _), reference in zip(instances, references)]])
            expr.append(symbol)
        for node, sheet_uuid, at, size, pins in sheet.sheets:
            block = [Sym('sheet'), [Sym('at'), at[0], at[1]], [Sym('size'), size[0], size[1]],
                     [Sym('stroke'), [Sym('width'), 0.1524], [Sym('type'), Sym('solid')]],
                     [Sym('fill'), [Sym('color'), 0, 0, 0, 0.0]], [Sym('uuid'), sheet_uuid],
                     [Sym('property'), 'Sheetname', sheet_name(node, sheet.sheets), [Sym('at'), at[0], at[1] - 0.7, 0],
                      FONT + [[Sym('justify'), Sym('left'), Sym('bottom')]]],
                     [Sym('property'), 'Sheetfile', node.component + '.kicad_sch', [Sym('at'), at[0], at[1] + size[1] + 0.6, 0],
                      FONT + [[Sym('justify'), Sym('left'), Sym('top')]]]]
            for pin, side, point in pins:
                block.append([Sym('pin'), kicad_name(pin), Sym(side), [Sym('at'), point[0], point[1], 0 if side == 'output' else 180],
                              [Sym('uuid'), sheet.uuid((sheet_uuid, pin))],
                              FONT + [[Sym('justify'), Sym('right' if side == 'output' else 'left')]]])
            block.append([Sym('instances'), [Sym('project'), self.name] +
                          [[Sym('path'), path, [Sym('page'), self.pages[path + '/' + sheet_uuid]]] for path, _ in instances]])
            expr.append(block)
        expr += sheet.items
        if top:
            expr.append([Sym('sheet_instances'), [Sym('path'), '/', [Sym('page'), '1']]])
        expr.append([Sym('embedded_fonts'), Sym('no')])
        return expr


# ---------------------------------------------------------------------------
# The round trip

def flatten(library, top, parts=()):
    """The source netlist as nets of primitive pins across the whole hierarchy. Pins are (path, node, pin);
    GND and VCC stand for Low and High nodes. Unconnected inputs read 0, so they join GND. A component
    parts.yaml maps to chips is a leaf, like a primitive: its port pins are the chips' pins."""
    parent = {}

    def root(key):
        parent.setdefault(key, key)
        while parent[key] != key:
            parent[key] = parent[parent[key]]
            key = parent[key]
        return key

    def union(a, b):
        parent[root(a)] = root(b)

    pins = set()

    def visit(name, path):
        component = library.load(name)
        wired = set()
        for net in component.nets:
            keys = []
            for node_id, pin in net:
                node = component.nodes[node_id]
                wired.add((node_id, pin))
                if node.kind in POWER_KINDS:
                    keys.append(POWER_KINDS[node.kind])
                elif node.kind in ('Input', 'Output'):
                    keys.append(('port', path, pin))
                elif node.kind == 'Component' and node.component not in parts:
                    keys.append(('port', path + (node_id,), pin))
                else:
                    keys.append((path, node_id, pin))
                    pins.add((path, node_id, pin))
            for key in keys:
                union(keys[0], key)
        for node in component.nodes.values():
            for pin in node.inputs:
                if (node.id, pin) in wired or node.kind == 'Output':
                    continue
                sheet = node.kind == 'Component' and node.component not in parts
                key = ('port', path + (node.id,), pin) if sheet else (path, node.id, pin)
                if not sheet:
                    pins.add(key)
                union(key, 'GND')
            if node.kind == 'Component' and node.component not in parts:
                visit(node.component, path + (node.id,))

    visit(top, ())
    component = library.load(top)
    pins.update(('port', (), pin) for kind in ('Input', 'Output') for pin in component.port_bits(kind))
    groups = {}
    for key in list(pins) + ['GND', 'VCC']:
        groups.setdefault(root(key), set()).add(key)
    return {frozenset(group) for group in groups.values() if len(group) > 1}


def read_kicad_netlist(path, pin_map):
    with open(path, encoding='utf-8') as file:
        nets = find(sexpr.parse(file.read()), 'nets')
    result = set()
    for net in find_all(nets, 'net'):
        name = find(net, 'name')[1]
        group = set()
        if name in ('GND', 'VCC', '/GND', '/VCC'):
            group.add(name.strip('/'))
        for node in find_all(net, 'node'):
            entry = pin_map.get('%s:%s' % (find(node, 'ref')[1], find(node, 'pin')[1]))
            if entry:
                group.add(('port', (), entry[1]) if entry[0] == 'port' else (tuple(entry[0]), entry[1], entry[2]))
        if len(group) > 1:
            result.add(frozenset(group))
    return result


def check(project, directory, cli):
    top = os.path.join(directory, project.name + '.kicad_sch')
    netlist_path = os.path.join(directory, project.name + '.net')
    subprocess.run([cli, 'sch', 'export', 'netlist', '--format', 'kicadsexpr', '-o', netlist_path, top],
                   check=True, capture_output=True)
    exported = read_kicad_netlist(netlist_path, project.pin_map)
    # Without a floorplan the top's ports are only labels, not on the connector, so its netlist can't show them.
    source = {group for group in (frozenset(key for key in group if project.floorplan or key[:2] != ('port', ()))
                                  for group in flatten(project.library, project.name, project.parts)) if len(group) > 1}
    def describe(group):
        return ', '.join(sorted(key if isinstance(key, str) else 'port ' + key[2] if key[0] == 'port' else
                                ''.join('%d/' % i for i in key[0]) + '%d.%s' % (key[1], key[2]) for key in group))
    missing, extra = source - exported, exported - source
    for group in sorted(missing, key=describe)[:10]:
        print('  in the netlist, not the schematic: ' + describe(group))
    for group in sorted(extra, key=describe)[:10]:
        print('  in the schematic, not the netlist: ' + describe(group))
    print('%s: %d nets, %d pins; %s' % (project.name, len(source), sum(len(g) for g in source),
                                         'schematic matches the netlist' if not (missing or extra) else
                                         '%d nets differ' % (len(missing) + len(extra))))

    report = os.path.join(directory, project.name + '.erc.json')
    subprocess.run([cli, 'sch', 'erc', '--format', 'json', '--severity-all', '-o', report, top],
                   capture_output=True)
    counts = {}
    with open(report, encoding='utf-8') as file:
        for sheet in json.load(file).get('sheets', []):
            for violation in sheet.get('violations', []):
                severity = violation['severity']
                if violation['type'] in UNMAPPED and not project.floorplan:
                    severity = 'expected'  # the card's ports reach nothing until the connector's pins are mapped
                key = (severity, violation['type'])
                counts[key] = counts.get(key, 0) + 1
    for (severity, kind), count in sorted(counts.items()):
        print('  ERC %s: %s x%d' % (severity, kind, count))
    errors = sum(count for (severity, _), count in counts.items() if severity == 'error')
    print('ERC: %d errors, %d warnings (%s)' % (errors, sum(counts.values()) - errors, report))
    return not (missing or extra) and errors == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('component', help='the top component, e.g. "Kestrel16 Computer"')
    parser.add_argument('-o', '--output', help='project directory (default Build/KiCad/<component>)')
    parser.add_argument('--components', default=None, help='where the netlists are (default Build/Components/)')
    parser.add_argument('--parts', default=os.path.join(HERE, 'parts.yaml'))
    parser.add_argument('--symbols', default=None, help="KiCad's symbol library directory")
    parser.add_argument('--kicad-cli', default=None)
    parser.add_argument('--floorplan', help="floorplan.py's pin map (default: the one in Build/Cards that has "
                        'this card, if any)')
    parser.add_argument('--straight', action='store_true', help="the floorplan's pins even if layout.py has dealt "
                        'the card its own')
    parser.add_argument('--check', action='store_true', help='round-trip the netlist through kicad-cli and run ERC')
    args = parser.parse_args()

    symbols_dir = args.symbols or os.environ.get('KICAD9_SYMBOL_DIR') or find_kicad('share/kicad/symbols')
    if not symbols_dir:
        sys.exit("can't find KiCad's symbol libraries; pass --symbols")
    with open(args.parts, encoding='utf-8') as file:
        parts = yaml.safe_load(file)
    library = Library(args.components) if args.components else Library()
    floorplan = load_floorplan(args.component, args.floorplan, args.straight)
    print('%s: %s' % (args.component, 'connector pins from the floorplan' if floorplan else
                      'no floorplan has this card, so its connector is left unconnected'))
    project = Project(args.component, library, parts, Symbols(symbols_dir), floorplan)
    directory = args.output or os.path.join(HERE, '..', '..', 'Build', 'KiCad', args.component)
    project.write(directory)
    print('%s: %d sheets, %d chips -> %s' % (args.component, len(project.sheets),
                                              sum(1 for (_, _, package), ref in project.references.items()
                                                  if not ref.startswith('#')), os.path.abspath(directory)))
    if args.check:
        if not check(project, directory, args.kicad_cli or kicad_cli()):
            sys.exit(1)


if __name__ == '__main__':
    main()
