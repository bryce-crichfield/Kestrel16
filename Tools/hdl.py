"""A small hardware description language for the generators, over circuit.Builder (which writes the YAML).

    with component('Mux 2x4', 'Four 2-input multiplexers: Yn = S ? Bn : An. Like a 74HC157.'):
        A, B = bus_input('A', 4), bus_input('B', 4)
        S = inputs('S')
        S_not = ~S
        output('Y', [mux(S, a, b, S_not) for a, b in zip(A, B)])

Wiring:
    a >> pin               wire one signal to a pin (or to every pin in a list: fan-out)
    bus >> pins            wire a bus bit by bit; the widths must match
    bus(a, [b, c], d)      join signals and buses into one Bus; Buses slice and add into Buses

Logic:
    a & b, a | b, a ^ b, ~a, nand(a, b), nor(a, b)
    mux(s, a, b)           s ? b : a, from gates (pass s_not to share one Not between muxes)
    any_of(signals)        an Or tree
    low(), high()          the component's constant 0 and 1 (one node each, shared)

Parts:
    part = Part('Register 16')     a saved component; its ports come from Build/Components/Register 16.yaml
    part.D, part['/LOAD']          a bus port is a Bus, a one-bit port a Signal; unknown names are errors
    flip_flop(clock), tristate(a, oe_not), rom(...), ram(...), clock_source(), primitive(kind)
    drive(bus, oe_not)             a Bus Driver 8 / 16 part: returns its Y
    mux_bus(s, a, b)               s ? b : a for a whole bus, through Mux 2x4 parts

Cards:
    with card('ALU'):      parts and gates made inside go on that card (the YAML node gets card: ALU).
        alu = Part('ALU 16')   Untagged nodes go on the card of the part that contains them, so tagging
                               the top-level blocks covers the design. Tools/eda/cards.py splits the nets.

Layout is automatic: when the component is saved, inputs go on the left, outputs on the right, and every
other node one column past the deepest node feeding it. Feedback from state (flip-flops, RAM and parts
that contain them) into the logic computing the next state doesn't count, so that logic sits to the left
of the state it feeds. Pass column= to a part to place it by hand.
"""
import os
import re
import sys
from contextlib import contextmanager
from circuit import Builder, COMPONENTS

__all__ = ['Signal', 'Bus', 'bus', 'Part', 'primitive', 'flip_flop', 'clock_source', 'tristate', 'rom', 'ram',
           'gate', 'nand', 'nor', 'mux', 'any_of', 'low', 'high', 'drive', 'mux_bus',
           'component', 'inputs', 'bus_input', 'output', 'card']

_circuit = None  # the component being described


# ---------------------------------------------------------------------------- signals and buses

class Signal(str):
    """One net, named the way the YAML names it: "12.Y", "3.D0", or "4" for a one-bit Input."""

    def __rshift__(self, target):
        for pin in [target] if isinstance(target, str) else target:
            _circuit.wire(self, pin)
        return target

    def __and__(self, other): return gate('And', self, other)
    def __or__(self, other): return gate('Or', self, other)
    def __xor__(self, other): return gate('Xor', self, other)
    def __invert__(self): return gate('Not', self)


class Bus(list):
    """Signals, bit 0 first. Slices and sums of Buses are Buses."""

    def __getitem__(self, index):
        result = list.__getitem__(self, index)
        return Bus(result) if isinstance(index, slice) else result

    def __add__(self, other): return Bus(list(self) + list(other))
    def __radd__(self, other): return Bus(list(other) + list(self))

    def __rshift__(self, targets):
        targets = list(targets)
        if len(targets) != len(self):
            raise ValueError('wiring %d bits to %d pins' % (len(self), len(targets)))
        for source, target in zip(self, targets):
            _circuit.wire(source, target)
        return targets

    def reversed(self):
        return Bus(self[::-1])


def bus(*pieces):
    """bus(low(), A[:8], [x, y]): signals and lists of signals joined into one Bus."""
    result = Bus()
    for piece in pieces:
        result += [piece] if isinstance(piece, str) else list(piece)
    return result


# ---------------------------------------------------------------------------- nodes

class Node:
    """A node: node['CLK'] is one pin, node.Y (or node['Y']) a bus port, all of Y0, Y1, ... as a Bus.
    If the node's pins are known (parts, ROM, RAM, flip-flops) other names are an error."""

    def __init__(self, node_id, pins=None):
        self.id, self._pins = node_id, pins

    def __getitem__(self, name):
        if self._pins is None or name in self._pins:
            return Signal('%d.%s' % (self.id, name))
        groups = {}
        for pin in self._pins:
            match = re.fullmatch(r'(.*?)(\d+)', pin)
            if match:
                groups.setdefault(match.group(1), {})[int(match.group(2))] = pin
        group = groups.get(name, {})
        if not group or sorted(group) != list(range(len(group))):
            raise KeyError('node %d has no pin or bus named %s' % (self.id, name))
        return Bus(Signal('%d.%s' % (self.id, group[i])) for i in range(len(group)))

    def __getattr__(self, name):
        if name.startswith('_'):
            raise AttributeError(name)
        return self[name]


_parts = {}


def _component_file(name):
    """A saved component's (input pins, output pins, holds state)."""
    if name not in _parts:
        with open(os.path.join(COMPONENTS, name + '.yaml')) as file:
            text = file.read()
        ports = {'Input': [], 'Output': []}
        for kind, label, width in re.findall(r'kind: (Input|Output), label: "?([^",}]+)"?(?:, width: (\d+))?', text):
            ports[kind] += ['%s%d' % (label, i) for i in range(int(width))] if width else [label]
        stateful = bool(re.search(r'kind: (FlipFlop|RAM)\b', text)) or any(
            _component_file(part)[2] for part in re.findall(r'component: ([^,}\n]+)', text))
        _parts[name] = (ports['Input'], ports['Output'], stateful)
    return _parts[name]


def Part(name, column=None):
    """An instance of a saved component."""
    inputs, outputs, stateful = _component_file(name)
    node_id = _circuit.add(lambda b, x: b.part(name, x, max(len(inputs), len(outputs))), stateful, column)
    return Node(node_id, inputs + outputs)


PRIMITIVE_PINS = {'FlipFlop': ['D', 'CLK', 'Q', '/Q'], 'TriState': ['A', '/OE', 'Y'],
                  'Terminal': ['RS', '/CE', '/OE', '/WE', 'IRQ'] + ['D%d' % i for i in range(8)]}


def primitive(kind, column=None):
    """A simulator primitive (FlipFlop, TriState, Clock, Terminal, ...)."""
    height = 400 if kind == 'Terminal' else 120
    node_id = _circuit.add(lambda b, x: b._add('kind: %s' % kind, x, height), kind == 'FlipFlop', column)
    return Node(node_id, PRIMITIVE_PINS.get(kind))


def flip_flop(clock):
    """A D flip-flop on clock: wire its next value to .D, read .Q and ['/Q']."""
    ff = primitive('FlipFlop')
    clock >> ff.CLK
    return ff


def clock_source():
    """The simulation clock."""
    return Signal(primitive('Clock').id)


def tristate(a, output_enable_not):
    """a while output_enable_not is 0, otherwise nothing."""
    buffer = primitive('TriState')
    a >> buffer.A
    output_enable_not >> buffer['/OE']
    return buffer.Y


def rom(address, data, image=None, rows=None, column=None):
    """A ROM with these address and data pin names, always enabled (/CE and /OE wired low)."""
    node_id = _circuit.add(lambda b, x: b.rom(x, address, data, image=image, rows=rows), False, column)
    chip = Node(node_id, address + data + ['/CE', '/OE'])
    low() >> [chip['/CE'], chip['/OE']]
    return chip


def ram(address_bits, image, lane, column=None):
    """An 8-bit-wide RAM: pins A0.., D0-D7, /CE, /OE, /WE."""
    node_id = _circuit.add(lambda b, x: b.ram(x, address_bits, image, lane), True, column)
    return Node(node_id, ['A%d' % i for i in range(address_bits)] + ['D%d' % i for i in range(8)] + ['/CE', '/OE', '/WE'])


# ---------------------------------------------------------------------------- logic

def gate(kind, *inputs):
    node_id = _circuit.add(lambda b, x: b.gate(kind, x))
    for pin, source in zip('AB', inputs):
        source >> Signal('%d.%s' % (node_id, pin))
    return Signal('%d.Y' % node_id)


def nand(a, b): return gate('Nand', a, b)
def nor(a, b): return gate('Nor', a, b)


def mux(select, if0, if1, select_not=None):
    """select ? if1 : if0. Pass select_not if the inverse already exists, to save a Not."""
    if select_not is None:
        select_not = ~select
    return (if0 & select_not) | (if1 & select)


def any_of(signals):
    """1 if any of signals is 1: a tree of Or gates."""
    terms = list(signals)
    while len(terms) > 1:
        terms = [terms[i] | terms[i + 1] if i + 1 < len(terms) else terms[i] for i in range(0, len(terms), 2)]
    return terms[0]


def _constant(kind):
    if kind not in _circuit.constants:
        _circuit.constants[kind] = Signal(_circuit.add(lambda b, x: b.gate(kind, x)))
    return _circuit.constants[kind]


def low(): return _constant('Low')
def high(): return _constant('High')


def drive(source, output_enable_not):
    """source onto a bus while output_enable_not is 0, through a Bus Driver 8 or 16. Returns its Y."""
    driver = Part('Bus Driver %d' % len(source))
    bus(source) >> driver.A
    output_enable_not >> driver['/OE']
    return driver.Y


def mux_bus(select, if0, if1):
    """select ? if1 : if0 for a whole bus, four bits per Mux 2x4. The shorter side is padded with 0s."""
    width = max(len(if0), len(if1))
    pad = lambda bits: bus(bits, [low()] * (-width % 4 + width - len(bits)))
    if0, if1, result = pad(if0), pad(if1), Bus()
    for start in range(0, width, 4):
        chip = Part('Mux 2x4')
        if0[start:start + 4] >> chip.A
        if1[start:start + 4] >> chip.B
        select >> chip.S
        result += chip.Y
    return result[:width]


# ---------------------------------------------------------------------------- components

class _Circuit:
    """The nodes and wires of the component being described. Nodes are placed when it's saved."""

    def __init__(self, name, description):
        self.name, self.description = name, description
        self.nodes, self.wires, self.constants = [], [], {}
        self.card = None  # the card new nodes go on, set by card()

    def add(self, emit, stateful=False, column=None, kind=None):
        """emit(builder, column) adds the node to a Builder (at save time) and returns its id."""
        self.nodes.append({'emit': emit, 'stateful': stateful, 'column': column, 'kind': kind, 'card': self.card})
        return len(self.nodes)

    def wire(self, source, target):
        self.wires.append((source, target))

    def columns(self):
        ids = range(1, len(self.nodes) + 1)
        info = lambda i: self.nodes[i - 1]
        feeders = {i: set() for i in ids}
        for source, target in self.wires:
            s, t = int(source.split('.')[0]), int(target.split('.')[0])
            if s != t:
                feeders[t].add(s)
        # The next-state logic: nodes that reach a state element's inputs.
        state = {i for i in ids if info(i)['stateful']}
        next_state, stack = set(state), list(state)
        while stack:
            for feeder in feeders[stack.pop()]:
                if feeder not in next_state:
                    next_state.add(feeder)
                    stack.append(feeder)

        level, visiting = {}, set()

        def depth(i):
            if i not in level:
                if info(i)['column'] is not None:
                    level[i] = info(i)['column']
                elif info(i)['kind'] == 'Input':
                    level[i] = 0
                else:
                    visiting.add(i)
                    level[i] = 1 + max((depth(f) for f in feeders[i] if f not in visiting
                                        and not (f in state and i in next_state)), default=0)
                    visiting.discard(i)
            return level[i]

        limit = sys.getrecursionlimit()
        sys.setrecursionlimit(max(limit, 10 * len(self.nodes) + 100))
        inner = [depth(i) for i in ids if info(i)['kind'] != 'Output']
        sys.setrecursionlimit(limit)
        last = max(inner, default=0) + 1
        return {i: last if info(i)['kind'] == 'Output' else level[i] for i in ids}

    def save(self):
        builder = Builder(self.name, self.description)
        columns = self.columns()
        for i, node in enumerate(self.nodes, 1):
            first = len(builder.nodes)  # the node's first line: all of it in flow style, or a block's id line
            emitted = node['emit'](builder, columns[i])
            assert int(str(emitted).split('.')[0]) == i
            if node['card']:
                tag = 'card: "%s"' % node['card']
                if builder.nodes[first].startswith('  - {'):
                    builder.nodes[first] = builder.nodes[first].replace(', ', ', %s, ' % tag, 1)
                else:
                    builder.nodes.insert(first + 1, '    ' + tag)
        builder.wires = ['%s -> %s' % wire for wire in self.wires]
        builder.save()


@contextmanager
def component(name, description):
    """Describe a component inside the with block; it's saved to Build/Components/<name>.yaml at the end."""
    global _circuit
    _circuit = _Circuit(name, description)
    try:
        yield
        _circuit.save()
    finally:
        _circuit = None


@contextmanager
def card(name):
    """Put the nodes made inside the with block on this card. Blocks don't nest."""
    if _circuit.card is not None:
        raise ValueError('card %s inside card %s' % (name, _circuit.card))
    _circuit.card = name
    try:
        yield
    finally:
        _circuit.card = None


def inputs(*names):
    """One-bit inputs, in order: a Signal for one name, a tuple of them for several."""
    signals = tuple(Signal(_circuit.add(lambda b, x, name=name: b.input(name, x), kind='Input')) for name in names)
    return signals[0] if len(signals) == 1 else signals


def bus_input(name, width):
    node_id = _circuit.add(lambda b, x: b.bus_input(name, width, x)[0], kind='Input')
    return Bus(Signal('%d.%s%d' % (node_id, name, i)) for i in range(width))


def output(name, source=None, width=None):
    """An output driven by source (a Signal or a list of them). Returns its pins, for more drivers.
    With no source, give width (0 for one bit) and wire the returned pins yourself."""
    width = width if source is None else (0 if isinstance(source, str) else len(source))
    if width:
        node_id = _circuit.add(lambda b, x: b.bus_output(name, width, x)[0], kind='Output')
        pins = Bus(Signal('%d.%s%d' % (node_id, name, i)) for i in range(width))
    else:
        pins = Signal(_circuit.add(lambda b, x: b.output(name, x), kind='Output'))
    if source is not None:
        (source if isinstance(source, str) else Bus(source)) >> pins
    return pins
