"""Reads the component netlists in Components/ into nets, the same way the simulator does.

A pin is (node id, pin name). Pin names follow Source/Simulator/Circuit.cpp: a wire endpoint like 7.A1..A15 is a
run of the node's pins by position, a bare id is a port's whole width or a one-pin node's only output, and a
RAM's or Terminal's data pins are one pin each though they appear on both sides.
"""
import os
import yaml

COMPONENTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'Build', 'Components')

GATE_PINS = {
    'Buffer': (['A'], ['Y']), 'Not': (['A'], ['Y']), 'And': (['A', 'B'], ['Y']), 'Or': (['A', 'B'], ['Y']),
    'Nand': (['A', 'B'], ['Y']), 'Nor': (['A', 'B'], ['Y']), 'Xor': (['A', 'B'], ['Y']), 'Xnor': (['A', 'B'], ['Y']),
    'FlipFlop': (['D', 'CLK'], ['Q', '/Q']), 'TriState': (['A', '/OE'], ['Y']),
    'Low': ([], ['0']), 'High': ([], ['1']), 'Clock': ([], ['clk']),
    'Terminal': (['RS', '/CE', '/OE', '/WE'] + ['D%d' % i for i in range(8)], ['D%d' % i for i in range(8)] + ['IRQ']),
}


def bits(label, width):
    return [label] if width == 1 else ['%s%d' % (label, bit) for bit in range(width)]


class Node:
    def __init__(self, entry, library):
        self.id = entry['id']
        self.at = tuple(entry.get('at', (0, 0)))
        self.kind = entry.get('kind', 'Component')
        self.component = entry.get('component')  # the child's name, for a Component node
        self.label = entry.get('label', '')
        self.width = entry.get('width', 1)
        self.entry = entry
        if self.kind == 'Input':
            self.inputs, self.outputs = [], bits(self.label, self.width)
        elif self.kind == 'Output':
            self.inputs, self.outputs = bits(self.label, self.width), []
        elif self.kind in ('ROM', 'RAM'):
            address = entry.get('inputs') or ['A%d' % i for i in range(entry['address'])]
            data = entry.get('outputs') or ['D%d' % i for i in range(entry['data'])]
            controls = ['/CE', '/OE'] + (['/WE'] if self.kind == 'RAM' else [])
            self.inputs = address + controls + (data if self.kind == 'RAM' else [])
            self.outputs = data
            self.address, self.data = address, data
        elif self.kind == 'Component':
            child = library.load(self.component)
            self.inputs, self.outputs = child.port_bits('Input'), child.port_bits('Output')
        elif self.kind in GATE_PINS:
            self.inputs, self.outputs = GATE_PINS[self.kind]
        else:
            raise ValueError('node %d: unknown kind %s' % (self.id, self.kind))
        # Wires name pins per side, so a ROM may have an address pin and a data pin both called OP0. Those
        # are two pins: the data one becomes OP0'. A RAM's or Terminal's D0 really is one pin on both sides.
        self.output_names = self.outputs
        clash = set(self.inputs) & set(self.outputs)
        if clash and self.kind not in ('RAM', 'Terminal'):
            if self.kind == 'Component':
                raise ValueError('%s has an input and an output both called %s' % (self.component, sorted(clash)[0]))
            self.outputs = [name + "'" if name in clash else name for name in self.outputs]
            if self.kind == 'ROM':
                self.data = self.outputs

    def pins(self):
        """Every pin name, each once."""
        return list(dict.fromkeys(self.inputs + self.outputs))


class Component:
    def __init__(self, name, library):
        with open(os.path.join(library.directory, name + '.yaml'), encoding='utf-8') as file:
            data = yaml.safe_load(file)
        self.name = data['name']
        self.nodes = {}
        for entry in data['nodes']:
            node = Node(entry, library)
            self.nodes[node.id] = node
        parent = {}

        def root(pin):
            parent.setdefault(pin, pin)
            while parent[pin] != pin:
                parent[pin] = parent[parent[pin]]
                pin = parent[pin]
            return pin

        for wire in data.get('wires') or []:
            source, target = (text.strip() for text in wire.split('->'))
            sources, targets = self._endpoint(source, True), self._endpoint(target, False)
            if len(sources) != len(targets):
                raise ValueError('%s: wire %s is %d bits on one end, %d on the other' % (name, wire, len(sources), len(targets)))
            for a, b in zip(sources, targets):
                parent[root(a)] = root(b)
        groups = {}
        for pin in parent:
            groups.setdefault(root(pin), []).append(pin)
        self.nets = sorted((sorted(pins) for pins in groups.values()), key=lambda pins: pins[0])

    def _endpoint(self, text, output):
        node_text, _, pin_text = text.partition('.')
        node = self.nodes[int(node_text)]
        names, keys = (node.output_names, node.outputs) if output else (node.inputs, node.inputs)
        if not pin_text:
            if node.kind in ('Input', 'Output') or len(names) == 1:
                return [(node.id, key) for key in keys]
            raise ValueError('%s: %s needs a pin name' % (self.name, text))
        first, _, last = pin_text.partition('..')
        start = names.index(first)
        end = names.index(last) if last else start
        return [(node.id, key) for key in keys[start:end + 1]]

    def ports(self, kind):
        return [node for node in self.nodes.values() if node.kind == kind]

    def port_bits(self, kind):
        return [name for node in self.ports(kind) for name in bits(node.label, node.width)]

    def children(self):
        return [node for node in self.nodes.values() if node.kind == 'Component']


class Library:
    def __init__(self, directory=COMPONENTS):
        self.directory = directory
        self.components = {}

    def load(self, name):
        if name not in self.components:
            self.components[name] = None  # catches a component that contains itself
            self.components[name] = Component(name, self)
        if self.components[name] is None:
            raise ValueError('%s contains itself' % name)
        return self.components[name]
