"""Lays out one card: places its chips, then moves its connector pins to the chips that use them.

    python Tools/eda/layout.py "Card Registers" [place.py's options, like --gap 7x3]

It runs, in order:
  1. kicad.py --straight: the card on the floorplan's pins, each bus together on one connector
  2. place.py
  3. deals the card's ports to the pins nearest the chips on them, from place.py's <card>.pulls.json: the assignment
     with the least total distance, so a chip on the left of the card gets pins on the left. They go to
     Build/Cards/<card> Pins.yaml, which kicad.py takes over the floorplan's pins from then on.
  4. kicad.py, now on those pins
  5. place.py --renet: the placed boards take the new nets on their connector pads; nothing moves

place.py places into the 6U Eurocard template unless its options name another --template.

A card's pins depend on its own placement only, so each card is laid out on its own, in any order. Its nets are then on
different pins at different cards, and the backplane crosses them over between slots. A card no floorplan has is
just placed. kicad.py runs under this Python and place.py under KiCad's (--kicad-python, default: found).
"""
import argparse
import json
import math
import os
import subprocess
import sys

import yaml
from scipy.optimize import linear_sum_assignment

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from floorplan import natural, signal_pins  # noqa: E402
from kicad import find_floorplan, kicad_name, pins_path  # noqa: E402
from kicad_env import find_kicad  # noqa: E402

TEMPLATE = os.path.join(HERE, 'templates', '6U_Eurocard', '6U_Eurocard.kicad_pcb')
KICAD = os.path.normpath(os.path.join(HERE, '..', '..', 'Build', 'KiCad'))


def run(line):
    print('> %s' % subprocess.list2cmdline(line), flush=True)
    if subprocess.call(line):
        raise SystemExit('%s failed' % os.path.basename(line[1]))


def reassign(component, plan):
    """{port: pin} for the card's ports, each near the chips that use it; plan is the floorplan's, which a port no
    chip pulls on aims at."""
    path = os.path.join(KICAD, component, component + '.pulls.json')
    if not os.path.isfile(path):
        raise SystemExit("place.py left no %s: is the card's template missing its connectors?" % path)
    with open(path, encoding='utf-8') as file:
        placed = json.load(file)
    where, pulls = placed['pins'], placed['pulls']
    free = [pin for pin in signal_pins() if pin in where]
    ports = sorted(plan, key=natural)
    aims = [pulls.get(kicad_name(port), where.get(plan[port])) for port in ports]
    aims = [aim or [sum(where[p][i] for p in free) / len(free) for i in (0, 1)] for aim in aims]
    cost = [[math.hypot(where[pin][0] - ax, where[pin][1] - ay) for pin in free] for ax, ay in aims]
    rows, columns = linear_sum_assignment(cost)
    pins = {ports[r]: free[c] for r, c in zip(rows, columns)}
    moved = sum(pins[port] != plan[port] for port in ports)
    before = sum(math.dist(where[plan[p]], a) for p, a in zip(ports, aims) if plan[p] in where)
    after = sum(math.dist(where[pins[p]], a) for p, a in zip(ports, aims))
    print('  %d of %d ports moved; pin to chips %.0f mm -> %.0f mm in all' % (moved, len(ports), before, after))
    return dict(sorted(pins.items(), key=lambda item: free.index(item[1])))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('component', help='the card, e.g. "Card Registers"')
    parser.add_argument('--kicad-python', help="KiCad's Python, which place.py needs (default: found)")
    args, place_options = parser.parse_known_args()
    kicad_python = args.kicad_python or find_kicad('bin/python.exe') or find_kicad('bin/python3')
    if not kicad_python:
        raise SystemExit("can't find KiCad's Python; pass --kicad-python")
    component, project = args.component, os.path.join(KICAD, args.component)
    schematic = [sys.executable, os.path.join(HERE, 'kicad.py'), component]
    if not any(option == '--template' or option.startswith('--template=') for option in place_options):
        place_options += ['--template', TEMPLATE]
    place = [kicad_python, os.path.join(HERE, 'place.py'), project] + place_options

    floorplan = find_floorplan(component)
    print('== 1. schematic on the floorplan\'s pins', flush=True)
    run(schematic + ['--straight'])
    print('== 2. place', flush=True)
    run(place)
    if floorplan is None:
        print('no floorplan has %s, so its pins stay as they are' % component)
        return
    print('== 3. pins by placement', flush=True)
    pins = reassign(component, floorplan[0])
    with open(pins_path(component), 'w', encoding='utf-8', newline='\n') as file:
        yaml.safe_dump(pins, file, sort_keys=False)
    print('  -> %s' % os.path.relpath(pins_path(component)))
    print('== 4. schematic on those pins', flush=True)
    run(schematic)
    print('== 5. new nets on the placed boards', flush=True)
    run([kicad_python, os.path.join(HERE, 'place.py'), project, '--renet'])


if __name__ == '__main__':
    main()
