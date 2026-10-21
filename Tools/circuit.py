"""Helpers for the generator scripts: build a component node by node and save it as YAML."""
import os
import re

COMPONENTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'Build', 'Components')


class Builder:
    """Nodes go in columns; each node is placed below the previous one in its column."""

    def __init__(self, name, description):
        self.name, self.description = name, description
        self.nodes, self.wires, self.next_id, self.column_y = [], [], 1, {}
        self.widths = {}  # bus Input/Output node -> its width

    def _add(self, text, column, height):
        node_id = self.next_id
        self.next_id += 1
        y = self.column_y.get(column, 40)
        self.column_y[column] = y + height
        self.nodes.append('  - {id: %d, %s, at: [%d, %d]}' % (node_id, text, 40 + 260 * column, y))
        return node_id

    def input(self, label, column=0):
        return self._add('kind: Input, label: "%s"' % label, column, 100)

    def output(self, label, column):
        return self._add('kind: Output, label: "%s"' % label, column, 100)

    def bus_input(self, label, width, column=0):
        """One Input node with width pins, label0.. Returns the pins as wire ends."""
        node = self._add('kind: Input, label: "%s", width: %d' % (label, width), column, 100)
        self.widths[node] = width
        return ['%d.%s%d' % (node, label, i) for i in range(width)]

    def bus_output(self, label, width, column):
        node = self._add('kind: Output, label: "%s", width: %d' % (label, width), column, 100)
        self.widths[node] = width
        return ['%d.%s%d' % (node, label, i) for i in range(width)]

    def gate(self, kind, column):
        return self._add('kind: %s' % kind, column, 120)

    def part(self, component, column, pins, name=None):
        """A component instance; pins is its larger pin count, for spacing. name says what this instance is for
        (\"IR\", \"R3\"), and becomes its schematic sheet's name and its chips' silkscreen."""
        return self._add('component: %s%s' % (component, ', name: "%s"' % name if name else ''), column,
                         70 + 22 * pins)

    def wire(self, source, target):
        self.wires.append('%s -> %s' % (source, target))

    def gate2(self, kind, column, a, b):
        """A two-input gate fed from a and b. Returns its output."""
        gate = self.gate(kind, column)
        self.wire(a, '%d.A' % gate)
        self.wire(b, '%d.B' % gate)
        return '%d.Y' % gate

    def gate1(self, kind, column, a):
        gate = self.gate(kind, column)
        self.wire(a, '%d.A' % gate)
        return '%d.Y' % gate

    def mux(self, column, a, b, select, select_not=None):
        """select ? b : a, from gates. Pass select_not to share one NOT between many muxes."""
        select_not = select_not or self.gate1('Not', column, select)
        return self.gate2('Or', column + 1, self.gate2('And', column, a, select_not), self.gate2('And', column, b, select))

    def or_tree(self, column, terms):
        while len(terms) > 1:
            terms = [self.gate2('Or', column, terms[i], terms[i + 1]) if i + 1 < len(terms) else terms[i]
                     for i in range(0, len(terms), 2)]
        return terms[0]

    def rom(self, column, address_names, data_names, image=None, rows=None, lane=None):
        node_id = self.next_id
        self.next_id += 1
        y = self.column_y.get(column, 40)
        self.column_y[column] = y + 90 + 22 * (len(address_names) + 2 + len(data_names))
        lines = ['  - id: %d' % node_id, '    kind: ROM', '    at: [%d, %d]' % (40 + 260 * column, y),
                 '    address: %d' % len(address_names), '    data: %d' % len(data_names),
                 '    inputs: [%s]' % ', '.join('"%s"' % name for name in address_names),
                 '    outputs: [%s]' % ', '.join('"%s"' % name for name in data_names)]
        if image:
            lines.append('    image: %s' % image)
        else:
            lines += ['    contents:'] + ['      - "%s"' % row for row in rows]
        self.nodes += lines
        return node_id

    def ram(self, column, address_bits, image, lane):
        node_id = self.next_id
        self.next_id += 1
        y = self.column_y.get(column, 40)
        self.column_y[column] = y + 90 + 22 * (address_bits + 3 + 8)
        self.nodes += ['  - id: %d' % node_id, '    kind: RAM', '    at: [%d, %d]' % (40 + 260 * column, y),
                       '    address: %d' % address_bits, '    data: 8', '    image: %s' % image, '    lane: %s' % lane]
        return node_id

    def bus_wires(self):
        """The wires, with ones that run side by side (5.Y0 -> 7.A0, 5.Y1 -> 7.A1, ...) joined
        into one bus wire, 5.Y0..Y1 -> 7.A0..A1. The app joins them the same way when it loads."""
        numbered = re.compile(r'^(\d+)\.(.*?\D)(\d+)$')
        runs, plain = {}, []
        for wire in self.wires:
            source, target = wire.split(' -> ')
            a, b = numbered.match(source), numbered.match(target)
            if not a or not b:
                plain.append(wire)
                continue
            key = (a.group(1), a.group(2), b.group(1), b.group(2), int(b.group(3)) - int(a.group(3)))
            runs.setdefault(key, set()).add(int(a.group(3)))
        wires = list(plain)
        for (source, source_name, target, target_name, offset), numbers in runs.items():
            numbers = sorted(numbers)
            start = 0
            for i in range(1, len(numbers) + 1):
                if i < len(numbers) and numbers[i] == numbers[i - 1] + 1:
                    continue
                first, last = numbers[start], numbers[i - 1]
                start = i
                wires.append('%s -> %s' % (self.end(source, source_name, first, last),
                                           self.end(target, target_name, first + offset, last + offset)))
        return wires

    def end(self, node, name, first, last):
        if first == last:
            return '%s.%s%d' % (node, name, first)
        if first == 0 and self.widths.get(int(node)) == last + 1:
            return node  # all of a bus node
        return '%s.%s%d..%s%d' % (node, name, first, name, last)

    def save(self):
        lines = ['name: %s' % self.name, 'description: "%s"' % self.description, 'nodes:'] + self.nodes
        lines += ['wires:'] + ['  - ' + wire for wire in self.bus_wires()]
        with open(os.path.join(COMPONENTS, self.name + '.yaml'), 'w', newline='\n') as file:
            file.write('\n'.join(lines) + '\n')
