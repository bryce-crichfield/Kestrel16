"""Plans the backplane: which slot each card goes in, and which connector pin carries each net between cards.

    python Tools/eda/floorplan.py ["Kestrel16 Computer"] [--order Registers,ALU,...]

It reads the crossing nets cards.py wrote to Build/Cards/<design>.yaml. Every card has the same two 96-pin
connectors, P1 and P2, and the backplane joins the slots side by side. A net that crosses between cards runs along
the backplane from the first slot that uses it to the last, on one pin number, so its trace is a straight line. Pins
are reused: a pin that carries one net between slots 1 and 3 can carry another between slots 4 and 6. So a pin's
row of the backplane is cut into segments, and a card in the middle of a segment it doesn't use leaves that pin
unconnected. A fully bussed backplane, every pin on every slot, would need a pin for every crossing net at once.

The slot order is the one that needs the fewest pins at its busiest slot, then the shortest segments: every order is
tried when there are 8 cards or fewer. The pins are dealt out segment by segment, earliest slot first, each taking
the first pin free there (which needs no more pins than the busiest slot has nets), with the bits of a bus in
order, so a bus lands on neighbouring pins. Each bus goes on the connector its cards have used least, so every card
fills P1 and P2 evenly rather than P1 first. Pins go across the rows first (a3, b3, c3, a4, ...), so a byte sits
compactly on the connector.

Power: positions 1 and 32 of every row are GND and positions 2 and 31 are VCC, on both connectors, so power enters
at both ends of each; the card's inner planes spread it.

Each card's own layout then moves its pins next to the chips that use them: see layout.py, whose
Build/Cards/<card> Pins.yaml kicad.py takes over this map's pins.

Writes Build/Cards/<design> Floorplan.yaml, which kicad.py reads to wire each card's connector to its ports:
  slots: the cards in slot order
  power: GND and VCC, each a list of pins
  cards: per card, port name -> pin (like P1.b7)
  backplane: per pin, its segments: the net, its first and last slot, and the cards on it
"""
import argparse
import itertools
import os
import re
import sys

import yaml

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cards import BUILD, port_names  # noqa: E402

CONNECTORS = ('P1', 'P2')
ROWS = 'abc'
POSITIONS = 32
POWER = {'GND': (1, 32), 'VCC': (2, 31)}  # positions, on every row of both connectors
EXHAUSTIVE = 8  # the most cards whose every slot order is tried


def power_pins():
    return {net: ['%s.%s%d' % (c, row, p) for c in CONNECTORS for p in positions for row in ROWS]
            for net, positions in POWER.items()}


def signal_pins():
    """Every pin not kept for power, across the rows first: P1.a3, P1.b3, P1.c3, P1.a4, ..."""
    taken = {p for positions in POWER.values() for p in positions}
    return ['%s.%s%d' % (c, row, p) for c in CONNECTORS for p in range(1, POSITIONS + 1) if p not in taken
            for row in ROWS]


def spans(nets, order):
    """Each net's (first slot, last slot) for a slot order."""
    slot = {card: i for i, card in enumerate(order)}
    return [(min(slot[c] for c in net), max(slot[c] for c in net)) for net in nets]


def cost(nets, order):
    """(nets at the busiest slot, total segment length): the pins an order needs, then how long its traces run."""
    span = spans(nets, order)
    busiest = max(sum(a <= s <= b for a, b in span) for s in range(len(order)))
    return busiest, sum(b - a for a, b in span)


def best_order(cards, nets):
    if len(cards) <= EXHAUSTIVE:
        orders = itertools.permutations(cards)
    else:  # too many to try all: keep the order given, and say so
        print('  %d cards: more than %d, so the slots keep their sorted order' % (len(cards), EXHAUSTIVE))
        orders = [tuple(cards)]
    return min(orders, key=lambda order: (cost(nets, order), order))


def natural(name):
    """A sort key that puts DI2 before DI10."""
    return [int(part) if part.isdigit() else part for part in re.split(r'(\d+)', name)]


def assign(segments, pins):
    """{segment index: pin} for segments [(first, last, name, cards)]: interval colouring, earliest first, each taking the
    first pin whose last segment ended before it starts. None if they don't fit."""
    free_from = [0] * len(pins)  # the first slot each pin is free again
    result, home = {}, {}  # home: each bus's connector
    load = {}  # (card, connector) -> pins it has there so far

    def free(p, first, connector):
        return free_from[p] <= first and pins[p].startswith(connector + '.')

    for i in sorted(range(len(segments)), key=lambda i: (segments[i][0], natural(segments[i][2]))):
        first, last, name, cards = segments[i]
        # A bus (its name less the bit number) goes on the connector its cards have used least so far, then the one
        # with more pins free, so each card's two connectors fill evenly; its bits stay together there, and the other
        # connector takes any that don't fit.
        bus = (re.sub(r'\d+$', '', name), first)
        if bus not in home:
            home[bus] = min(CONNECTORS, key=lambda c: (sum(load.get((card, c), 0) for card in cards),
                                                       -sum(free(p, first, c) for p in range(len(pins)))))
        connectors = [home[bus]] + [c for c in CONNECTORS if c != home[bus]]
        pin = next((p for c in connectors for p in range(len(pins)) if free(p, first, c)), None)
        if pin is None:
            return None
        result[i] = pins[pin]
        free_from[pin] = last + 1
        for card in cards:
            connector = pins[pin].split('.')[0]
            load[(card, connector)] = load.get((card, connector), 0) + 1
    return result


def floorplan(design, order=None):
    path = os.path.join(BUILD, design + '.yaml')
    if not os.path.isfile(path):
        raise SystemExit("%s doesn't exist: run cards.py \"%s\" first" % (os.path.relpath(path), design))
    with open(path, encoding='utf-8') as file:
        report = yaml.safe_load(file)
    crossing = report['nets']
    cards = sorted(report['cards'])
    if len(cards) < 2:
        raise SystemExit('%s has %d cards: floorplan.py plans the whole computer, so give it the top design (the '
                         'default, "Kestrel16 Computer"), not one card' % (design, len(cards)))
    members = [sorted(set(net['drivers']) | set(net['listeners'])) for net in crossing]
    if order:
        unknown = set(order) ^ set(cards)
        if unknown:
            raise SystemExit('--order must name every card once: %s' % ', '.join(sorted(unknown)))
    else:
        order = best_order(cards, members)
    order = list(order)
    busiest, length = cost(members, order)
    port = port_names(crossing)

    span = spans(members, order)
    segments = [(a, b, port[net['name']], cards_on) for (a, b), net, cards_on in zip(span, crossing, members)]
    pins = signal_pins()
    assigned = assign(segments, pins)
    if assigned is None:
        raise SystemExit('%d nets cross the busiest slot, but there are only %d signal pins' % (busiest, len(pins)))

    plan = {card: {} for card in order}
    backplane = {}
    for i, (net, cards_on) in enumerate(zip(crossing, members)):
        pin = assigned[i]
        for card in cards_on:
            plan[card][port[net['name']]] = pin
        backplane.setdefault(pin, []).append({'net': port[net['name']], 'slots': [span[i][0] + 1, span[i][1] + 1],
                                              'cards': cards_on})
    used = sorted(set(assigned.values()), key=pins.index)
    result = {'design': design, 'slots': order, 'power': power_pins(),
              'cards': {card: dict(sorted(plan[card].items(), key=lambda item: pins.index(item[1]))) for card in order},
              'backplane': {pin: sorted(backplane[pin], key=lambda s: s['slots']) for pin in used}}
    return result, busiest, length


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('design', nargs='?', default='Kestrel16 Computer')
    parser.add_argument('--order', help='the cards in slot order, comma separated (default: the order needing the '
                        'fewest pins)')
    args = parser.parse_args()
    order = [card.strip() for card in args.order.split(',')] if args.order else None
    plan, busiest, length = floorplan(args.design, order)

    pins = len(signal_pins())
    print('slots: %s' % ', '.join('%d %s' % (i + 1, card) for i, card in enumerate(plan['slots'])))
    report(plan, pins, busiest, length)
    path = os.path.join(BUILD, args.design + ' Floorplan.yaml')
    with open(path, 'w', encoding='utf-8', newline='\n') as file:
        yaml.safe_dump(plan, file, sort_keys=False, width=120)
    print('wrote %s' % os.path.relpath(path))


def report(plan, pins, busiest, length):
    print('%d crossing nets on %d pins of %d (%d at the busiest slot); segments span %d slot gaps in all'
          % (sum(len(s) for s in plan['backplane'].values()), len(plan['backplane']), pins, busiest, length))
    for i, card in enumerate(plan['slots']):
        through = sum(1 for segments in plan['backplane'].values() for s in segments
                      if s['slots'][0] <= i + 1 <= s['slots'][1] and card not in s['cards'])
        print('  %-10s %3d pins used, %2d passing through' % (card, len(plan['cards'][card]), through))


if __name__ == '__main__':
    main()
