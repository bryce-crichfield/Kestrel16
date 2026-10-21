"""Splits a design into cards and reports the nets that cross between them.

    python Tools/eda/cards.py ["Kestrel16 Computer"] [--reserved 16] [--show 20]

Cards come from the card: key that hdl.card() writes on nodes. A node without one goes on the card of the part
that contains it, so tagging top-level blocks is enough; the innermost tag wins. The design is flattened to its
primitives (gates, flip-flops, ROMs, RAMs, ...), and every net is either local to one card or crosses: then it
needs a pin on each card's connector and a trace on the backplane.

For each crossing net the report says which cards drive it and which listen. It also flags primitives with no
card, crossing nets with no driver, and nets that several cards drive without tri-state outputs. Nets from
low() and high() aren't counted: each card ties its own pins to GND or VCC.

Writes Build/Cards/<design>.yaml: per card its pin count, and per crossing net its drivers and listeners. And
writes each card as a component, Build/Components/Card <name>.yaml, for kicad.py: its ports are the nets that cross
its edge (an Output where the card drives the net, an Input where it only listens), and the rest is the card's
part of the design. A part entirely on the card stays one part, so it keeps its own sheet; a part split across
cards (the CPU) is opened up and its pieces go on their cards. A card that uses low() or high() gets its own.
"""
import argparse
import os
import sys
import yaml
from netlist import COMPONENTS, Library, bits

BUILD = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'Build', 'Cards')
PINS = 96
CONSTANTS = ('Low', 'High')
# Primitives whose outputs can share a net with another driver.
TRISTATE = ('TriState', 'RAM', 'Terminal')


class Design:
    def __init__(self, top, library):
        self.library, self.top = library, top
        self.parent = {}
        self.leaves = {}  # (path, node id) -> (kind, card, output pins, where)
        self.nodes = {}  # (path, node id) -> (netlist Node, its card), for every node but ports
        self.children = {(): []}  # path -> the node ids in that component, ports left out
        self.ports = {}  # pin key -> readable name, for ports
        self._flatten(library.load(top), (), None, [])
        groups = {}
        for pin in list(self.parent):
            groups.setdefault(self._root(pin), []).append(pin)
        self.nets = list(groups.values())

    def _root(self, pin):
        self.parent.setdefault(pin, pin)
        while self.parent[pin] != pin:
            self.parent[pin] = self.parent[self.parent[pin]]
            pin = self.parent[pin]
        return pin

    def _join(self, a, b):
        self.parent[self._root(a)] = self._root(b)

    def _flatten(self, component, path, card, names):
        for node in component.nodes.values():
            here = node.entry.get('card', card)
            if node.kind not in ('Input', 'Output'):
                self.nodes[(path, node.id)] = (node, here)
                self.children.setdefault(path, []).append(node.id)
            if node.kind == 'Component':
                child = self.library.load(node.component)
                inner = path + (node.id,)
                ports = {}
                for port in child.ports('Input') + child.ports('Output'):
                    for name in bits(port.label, port.width):
                        ports[name] = port.id
                for name in node.pins():
                    self._join((path, node.id, name), (inner, ports[name], name))
                self._flatten(child, inner, here, names + ['%s %d' % (child.name, node.id)])
            elif node.kind in ('Input', 'Output'):
                for name in node.pins():
                    # An Input below the top names what the part receives: CLK rather than the stretcher's Y.
                    receives = node.kind == 'Input' and bool(path)
                    self.ports[(path, node.id, name)] = ('/'.join(names + [name]), receives)
            else:
                self.leaves[(path, node.id)] = (node.kind, here, set(node.outputs), '/'.join(names + [component.name]))
        for net in component.nets:
            pins = [(path,) + pin for pin in net]
            for pin in pins:
                self._root(pin)
            for pin in pins[1:]:
                self._join(pins[0], pin)

    def name(self, net):
        """The net's name at the highest level that has a port on it."""
        named = [self.ports[pin] for pin in net if pin in self.ports]
        if named:
            return min(named, key=lambda port: (port[0].count('/'), not port[1], port[0]))[0]
        path, node, pin = min(net)
        return '%s %d.%s' % (self.leaves[(path, node)][3], node, pin)


def partition(design):
    """(crossing nets, untagged leaves, problems). A crossing net is a dict: name, drivers, listeners."""
    crossing, problems = [], []
    untagged = sorted({leaf[3] for leaf in design.leaves.values() if leaf[1] is None})
    for net in design.nets:
        drivers, listeners, kinds = set(), set(), set()
        for path, node, pin in net:
            leaf = design.leaves.get((path, node))
            if leaf is None or leaf[1] is None:
                continue
            kind, card, outputs, _ = leaf
            if pin in outputs:
                drivers.add(card)
                kinds.add(kind)
            if pin not in outputs or kind in ('RAM', 'Terminal'):
                listeners.add(card)
        if kinds & set(CONSTANTS):
            continue
        cards = drivers | listeners
        if len(cards) < 2:
            continue
        name = design.name(net)
        if not drivers:
            problems.append('%s crosses %s but nothing drives it' % (name, ', '.join(sorted(cards))))
        elif len(drivers) > 1 and not kinds <= set(TRISTATE):
            problems.append('%s is driven by %s, not all tri-state' % (name, ', '.join(sorted(drivers))))
        crossing.append({'name': name, 'drivers': sorted(drivers), 'listeners': sorted(listeners - drivers)})
    crossing.sort(key=lambda net: net['name'])
    return crossing, untagged, problems


def split_name(name):
    """('System Slot 10', '/CS') from 'System Slot 10//CS': the owner's path, and the pin, which may start with /."""
    head, _, pin = name.rpartition('/')
    if head.endswith('/') or (not head and name.startswith('/')):
        head, pin = head[:-1], '/' + pin
    return head, pin


def port_names(crossing):
    """A short port name for each crossing net, the same on every card: the net's own name (DI0, /OE) unless
    two nets share it, then with its part's name in front (/System_Slot_CS)."""
    short = {net['name']: split_name(net['name'])[1] for net in crossing}
    count = {}
    for name in short.values():
        count[name] = count.get(name, 0) + 1
    def prefixed(full, numbered):
        name, owner = short[full], split_name(full)[0].split('/')[-1] or 'top'
        if not numbered:
            owner = owner.rsplit(' ', 1)[0]
        return '%s%s_%s' % ('/' if name.startswith('/') else '', owner.replace(' ', '_'), name.lstrip('/'))

    names = {full: name if count[name] == 1 else prefixed(full, False) for full, name in short.items()}
    seen = {}
    for name in names.values():
        seen[name] = seen.get(name, 0) + 1
    for full in names:  # Two of a part (Bus Driver 16 17 and 18): keep the instance number.
        if seen[names[full]] > 1:
            names[full] = prefixed(full, True)
    if len(set(names.values())) < len(names):
        raise SystemExit('two crossing nets got the same port name')
    return names


def card_components(design, crossing):
    """{card: component file contents}: each card's units (whole parts and loose primitives) and ports."""
    under = {}  # path -> the cards of the primitives below it
    for (path, _), leaf in design.leaves.items():
        for depth in range(len(path) + 1):
            under.setdefault(path[:depth], set()).add(leaf[1])
    units = {}  # card -> [(path, node id)]

    def visit(path):
        for node_id in design.children.get(path, []):
            node, card = design.nodes[(path, node_id)]
            inside = under.get(path + (node_id,), set()) if node.kind == 'Component' else {card}
            if node.kind == 'Component' and len(inside) > 1:
                visit(path + (node_id,))
            elif inside:
                units.setdefault(next(iter(inside)), []).append((path, node_id))

    visit(())
    roots = {}
    for net in design.nets:
        for pin in net:
            roots[pin] = id(net)
    names = {id(net): design.name(net) for net in design.nets}
    port = port_names(crossing)
    on_card = {net['name']: set(net['drivers']) | set(net['listeners']) for net in crossing}
    constant = {}  # net -> Low or High
    for net in design.nets:
        for path, node, _ in net:
            leaf = design.leaves.get((path, node))
            if leaf and leaf[0] in CONSTANTS:
                constant[id(net)] = leaf[0]

    files = {}
    for card, members in sorted(units.items()):
        entries, ids, drivers, sinks = [], {}, {}, {}
        for unit in members:
            node, _ = design.nodes[unit]
            ids[unit] = len(entries) + 1
            entry = {key: value for key, value in node.entry.items() if key not in ('id', 'at', 'card')}
            entries.append(dict({'id': ids[unit]}, **entry))
            for name in node.pins():
                net = roots.get(unit + (name,))
                if net is None:
                    continue
                if name in node.outputs:
                    text = node.output_names[node.outputs.index(name)]
                    drivers.setdefault(net, []).append('%d.%s' % (ids[unit], text))
                if name in node.inputs:
                    sinks.setdefault(net, []).append('%d.%s' % (ids[unit], name))
        ports = []
        for net in sorted(set(drivers) | set(sinks), key=lambda net: names[net]):
            name = names[net]
            if net in constant and net not in drivers:
                entries.append({'id': len(entries) + 1, 'kind': constant[net]})
                drivers[net] = ['%d' % len(entries)]
            elif name in port and card in on_card[name]:
                kind = 'Output' if net in drivers else 'Input'
                entries.append({'id': len(entries) + 1, 'kind': kind, 'label': port[name]})
                (sinks if kind == 'Output' else drivers).setdefault(net, []).append('%d' % len(entries))
                ports.append((kind, port[name]))
        wires = []
        for net, outs in drivers.items():
            ins = sinks.get(net, [])
            if ins:
                wires += ['%s -> %s' % (outs[0], target) for target in ins]
                wires += ['%s -> %s' % (source, ins[0]) for source in outs[1:]]
        # Lay it out as the generators do: inputs, then the parts in columns, then outputs, each node spaced by
        # its pin count. Overlapping symbols would join pins on the schematic.
        heights = [100 if entry.get('kind') in ('Input', 'Output', 'Low', 'High') else 0 for entry in entries]
        for unit in members:
            node, _ = design.nodes[unit]
            heights[ids[unit] - 1] = 90 + 22 * max(len(node.inputs), len(node.outputs), 1)
        inner = [i for i, entry in enumerate(entries) if entry.get('kind') not in ('Input', 'Output')]
        per_column = max(4000, sum(heights[i] for i in inner) // 8)
        column, y = 1, 40
        for i in inner:
            if y > 40 and y + heights[i] > per_column:
                column, y = column + 1, 40
            entries[i]['at'] = [40 + 260 * column, y]
            y += heights[i]
        for kind, x in (('Input', 0), ('Output', column + 1)):
            y = 40
            for entry, height in zip(entries, heights):
                if entry.get('kind') == kind:
                    entry['at'] = [40 + 260 * x, y]
                    y += height
        description = ('The %s card of %s, made by Tools/eda/cards.py. Its ports are the nets that cross the card '
                       'edge: %d in, %d out.' % (card, design.top, sum(k == 'Input' for k, _ in ports),
                                                 sum(k == 'Output' for k, _ in ports)))
        files[card] = {'name': 'Card %s' % card, 'description': description, 'nodes': entries, 'wires': wires}
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('design', nargs='?', default='Kestrel16 Computer')
    parser.add_argument('--components', default=None, help='where the netlists are (default Build/Components/)')
    parser.add_argument('--reserved', type=int, default=16, help='connector pins kept for VCC and GND')
    parser.add_argument('--show', type=int, default=15, help='how many of the widest-reaching nets to list')
    args = parser.parse_args()
    design = Design(args.design, Library(args.components) if args.components else Library())
    crossing, untagged, problems = partition(design)

    cards = sorted({leaf[1] for leaf in design.leaves.values() if leaf[1]})
    chips = {card: sum(1 for leaf in design.leaves.values() if leaf[1] == card) for card in cards}
    budget = PINS - args.reserved
    report = {}
    print('%-12s %10s %5s %5s %5s %6s' % ('card', 'primitives', 'in', 'out', 'both', 'pins'))
    for card in cards:
        out = sum(1 for net in crossing if card in net['drivers'] and len(net['drivers']) == 1)
        both = sum(1 for net in crossing if card in net['drivers'] and len(net['drivers']) > 1)
        into = sum(1 for net in crossing if card in net['listeners'])
        pins = out + both + into
        report[card] = {'primitives': chips[card], 'in': into, 'out': out, 'shared': both, 'pins': pins}
        print('%-12s %10d %5d %5d %5d %6d%s' % (card, chips[card], into, out, both, pins,
                                                '  over %d' % budget if pins > budget else ''))
    print('\n%d crossing nets; %d signal pins per card (%d reserved for power)' % (len(crossing), budget, args.reserved))

    wide = sorted(crossing, key=lambda net: (-len(net['drivers']) - len(net['listeners']), net['name']))
    print('\nReaching the most cards (candidates for the system bus, or for encoding):')
    for net in wide[:args.show]:
        print('  %-40s %s -> %s' % (net['name'], ','.join(net['drivers']) or '-', ','.join(net['listeners'])))
    for card in untagged:
        print('no card: %s' % card)
    for problem in problems:
        print('problem: %s' % problem)

    for card, data in card_components(design, crossing).items():
        path = os.path.join(design.library.directory, data['name'] + '.yaml')
        with open(path, 'w', encoding='utf-8', newline='\n') as file:
            yaml.safe_dump(data, file, sort_keys=False, width=1000, allow_unicode=True)
        print('wrote %s: %d nodes' % (os.path.relpath(path), len(data['nodes'])))

    os.makedirs(BUILD, exist_ok=True)
    path = os.path.join(BUILD, args.design + '.yaml')
    with open(path, 'w', encoding='utf-8', newline='\n') as file:
        yaml.safe_dump({'design': args.design, 'cards': report, 'nets': crossing}, file, sort_keys=False, width=120)
    print('\nwrote %s' % os.path.relpath(path))
    return 1 if untagged or problems else 0


if __name__ == '__main__':
    sys.exit(main())
