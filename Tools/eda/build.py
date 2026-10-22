"""The EDA pipeline: every stage, what each needs, and whether it's up to date.

    python Tools/eda/build.py                          # every stage's status
    python Tools/eda/build.py --run                    # run whatever isn't up to date, in order
    python Tools/eda/build.py Registers --until Route  # what one card's route needs, then the route (with --run)
    python Tools/eda/build.py ALU --stage Layout --run # one stage, run even if up to date, after what it needs

The parts file (parts_soic.yaml or parts_dip.yaml, under Tools/eda) is a setting for the whole computer: it says
which chips and packages the cards are built from, and changing it makes every Layout stale.

Generate, Cards and Floorplan run once for the computer; Layout, Congestion and Route run on each card. A stage is up
to date when it last succeeded after everything it needs, and on the same command line, so changing a setting makes
it stale. Each success leaves a stamp, Build/Stamps/<stage>/<target>.json, with its command line and time. Settings
are the ones gui.py saves, in Build/eda_gui.json, so the window and this agree on what's stale.
"""
import argparse
import json
import os
import subprocess
import sys
import time

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
TOOLS = os.path.join(ROOT, 'Tools')
SETTINGS = os.path.join(ROOT, 'Build', 'eda_gui.json')
PARTS = 'parts_soic.yaml'  # the parts file, under Tools/eda, unless the settings pick another
STAMPS = os.path.join(ROOT, 'Build', 'Stamps')
TEMPLATE = os.path.join(HERE, 'templates', '6U_Eurocard', '6U_Eurocard.kicad_pcb')
TOP = 'Kestrel16 Computer'
COMPUTER = 'Computer'  # the target of the stages that run once
GENERATORS = ['make_registers.py', 'make_alu.py', 'make_cpu.py']  # in order: later ones use earlier parts

PLACE = [
    ('--template', 'Template board', 'file', TEMPLATE),
    ('--gap', 'Slot room past each chip, along x across, mm', str, '7x4'),
    ('--packing', 'Packing: auto, rows or free', str, 'auto'),
    ('--vertical', 'Chips stand up (rows become columns)', bool, False),
    ('--label', "Card label (blank: project name; none: no label)", str, ''),
    ('--label-corner', 'Label corner: auto, top-left, top-right, bottom-left, bottom-right', str, 'auto'),
    ('--label-angle', 'Label rotation, degrees', str, '0'),
    ('--label-size', 'Label text height, mm', str, '3'),
    ('--back-labels', 'Chip labels on the back too', bool, False),
    ('--cohesion', 'Pull of each block on its chips (0: off)', str, '3'),
    ('--inflate', 'Rounds growing congested chips and placing again (0: off)', int, 0),
    ('--signal-layers', 'Signal layers: 2 (VCC plane on In2) or 3 (In2 signals, VCC routed)', int, 2),
    ('--runs', 'Anneals', int, 6),
    ('--candidates', 'Placements kept for routing', int, 5),
    ('--moves', 'Moves per anneal (blank: 4000 per chip)', str, ''),
    ('--seed', 'Seed', int, 1)]


class Stage:
    """script: under Tools/eda (None: the generators); python: 'python' (this one) or 'kicad'; argument: 'top' (the
    computer), 'component' (Card X), 'project' (its KiCad directory) or None; options: (flag, label, kind, default),
    a bool kind being a plain flag; needs: the stages before it; inputs: files it reads that no stage makes."""

    def __init__(self, name, script, python, argument, options=(), needs=(), card=False, inputs=()):
        self.name, self.script, self.python, self.argument = name, script, python, argument
        self.options, self.needs, self.card, self.inputs = list(options), list(needs), card, list(inputs)


STAGES = [
    Stage('Generate', None, 'python', None,
          inputs=[os.path.join(TOOLS, f) for f in GENERATORS + ['circuit.py', 'definitions.py', 'hdl.py']]),
    Stage('Cards', 'cards.py', 'python', 'top', [
        ('--reserved', 'Pins kept for power', int, 24),
        ('--show', 'Widest nets to list', int, 15)], needs=['Generate']),
    Stage('Floorplan', 'floorplan.py', 'python', None, [
        ('--order', 'Slot order, comma separated (blank: fewest pins)', str, '')], needs=['Cards']),
    Stage('Layout', 'layout.py', 'python', 'component', PLACE, needs=['Floorplan'], card=True),
    Stage('Congestion', 'congestion.py', 'kicad', 'project', [('--tile', 'Tile size, mm', str, '2.54')],
          needs=['Layout'], card=True),
    Stage('Route', 'route.py', 'kicad', 'project', [
        ('--candidates', 'Candidates to route, best first', int, 1),
        ('--passes', 'Routing passes per candidate', int, 25),
        ('--threads', 'Freerouting threads', int, 4),
        ('--timeout', 'Seconds per candidate', int, 600),
        ('--java', 'Java 21+ (blank: find it)', 'file', '')], needs=['Layout'], card=True),
]
BY_NAME = {stage.name: stage for stage in STAGES}


# ---------------------------------------------------------------------------------------------------- settings

def load_settings():
    try:
        with open(SETTINGS, encoding='utf-8') as file:
            settings = json.load(file)
    except (OSError, ValueError):
        settings = {}
    stages = settings.setdefault('stages', {})
    if 'Layout' not in stages and 'Place' in stages:  # the window's old Place settings
        stages['Layout'] = dict(stages['Place'])
    return settings


def save_settings(settings):
    try:
        os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
        with open(SETTINGS, 'w', encoding='utf-8') as file:
            json.dump(settings, file, indent=2)
    except OSError:
        pass


def option(settings, stage, flag, default):
    return settings.get('stages', {}).get(stage, {}).get(flag, default)


def parts_files():
    """The parts files to pick from, by name."""
    return sorted(name for name in os.listdir(HERE) if name.startswith('parts_') and name.endswith('.yaml'))


def parts_file(settings):
    """The parts file the settings pick, as a path."""
    return os.path.join(HERE, settings.get('parts') or PARTS)


def kicad_python(settings):
    if settings.get('kicad_python'):
        return settings['kicad_python']
    sys.path.insert(0, HERE)
    from kicad_env import find_kicad
    return find_kicad('bin/python.exe') or find_kicad('bin/python3') or 'python'


# ---------------------------------------------------------------------------------------------------- the graph

def cards():
    """The cards, as cards.py found them, or none before it has run."""
    try:
        with open(os.path.join(ROOT, 'Build', 'Cards', TOP + '.yaml'), encoding='utf-8') as file:
            return sorted(yaml.safe_load(file)['cards'])
    except (OSError, ValueError, KeyError, TypeError):
        return []


def targets():
    """Every (stage, target) in the order they run: the computer's stages, then each card's."""
    return ([(s.name, COMPUTER) for s in STAGES if not s.card] +
            [(s.name, card) for card in cards() for s in STAGES if s.card])


def needs(stage, target):
    return [(need, target if BY_NAME[need].card else COMPUTER) for need in BY_NAME[stage].needs]


def project(card):
    return os.path.join(ROOT, 'Build', 'KiCad', 'Card ' + card)


def commands(stage, target, settings):
    """The command lines a stage runs, as (python kind, script and arguments)."""
    s = BY_NAME[stage]
    if s.script is None:
        return [('python', [os.path.join(TOOLS, name)]) for name in GENERATORS]
    line = [os.path.join(HERE, s.script)]
    line += {'top': [TOP], 'component': ['Card ' + target], 'project': [project(target)]}.get(s.argument, [])
    for flag, _, kind, default in s.options:
        value = option(settings, stage, flag, default)
        if kind is bool:
            if value:
                line.append(flag)
        elif str(value).strip():
            line += [flag, str(value).strip()]
    if s.script == 'layout.py':
        line += ['--parts', os.path.basename(parts_file(settings))]
    return [(s.python, line)]


def command_lines(stage, target, settings):
    """The same, ready to run."""
    python = {'python': sys.executable, 'kicad': kicad_python(settings)}
    lines = []
    for kind, line in commands(stage, target, settings):
        if BY_NAME[stage].script == 'layout.py':
            line = line + ['--kicad-python', python['kicad']]
            line[line.index('--parts') + 1] = parts_file(settings)
        lines.append([python[kind]] + line)
    return lines


# ---------------------------------------------------------------------------------------------------- stamps

def stamp_path(stage, target):
    return os.path.join(STAMPS, stage, target + '.json')


def read_stamp(stage, target):
    try:
        with open(stamp_path(stage, target), encoding='utf-8') as file:
            return json.load(file)
    except (OSError, ValueError):
        return None


def stamp(stage, target, settings):
    """Records that a stage just succeeded."""
    path = stamp_path(stage, target)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as file:
        json.dump({'time': time.time(), 'command': commands(stage, target, settings)}, file, indent=1)


def inputs(stage, settings):
    s = BY_NAME[stage]
    files = list(s.inputs) + ([parts_file(settings)] if s.script == 'layout.py' else [])
    files += [option(settings, stage, flag, default) for flag, _, kind, default in s.options if kind == 'file']
    return [f for f in files if f and os.path.isfile(f)]


def status(stage, target, settings, memo=None):
    """('done' | 'stale' | 'missing', why)."""
    memo = {} if memo is None else memo
    if (stage, target) in memo:
        return memo[(stage, target)]
    stamp = read_stamp(stage, target)
    if stamp is None:
        result = ('missing', 'not run yet')
    elif stamp['command'] != [list(c) for c in commands(stage, target, settings)]:
        result = ('stale', 'its settings changed')
    else:
        result = ('done', 'up to date')
        for need in needs(stage, target):
            state, _ = status(*need, settings, memo)
            need_stamp = read_stamp(*need)
            if state != 'done' or need_stamp['time'] > stamp['time']:
                result = ('stale', '%s changed since' % need[0] if state == 'done' else '%s is %s' %
                          (need[0], 'not run' if state == 'missing' else 'stale'))
                break
        else:
            for f in inputs(stage, settings):
                if os.path.getmtime(f) > stamp['time']:
                    result = ('stale', '%s changed since' % os.path.relpath(f, ROOT))
                    break
    memo[(stage, target)] = result
    return result


def plan(settings, wanted=None, force=()):
    """The (stage, target)s to run, in order: those of wanted (default: every target) that aren't up to date, the ones
    they need that aren't, and every one in force."""
    memo, chosen = {}, []

    def visit(item):
        for need in needs(*item):
            visit(need)
        if item not in chosen and (item in force or status(*item, settings, memo)[0] != 'done'
                                   or any(need in chosen for need in needs(*item))):
            chosen.append(item)

    for item in wanted if wanted is not None else targets():
        visit(item)
    return chosen


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('cards', nargs='*', help='cards to build (default: the whole computer)')
    parser.add_argument('--until', choices=[s.name for s in STAGES], help="a stage and what it needs (default: all)")
    parser.add_argument('--stage', choices=[s.name for s in STAGES], help='run this stage even if up to date')
    parser.add_argument('--run', action='store_true', help="run what isn't up to date (default: only show it)")
    args = parser.parse_args()
    settings = load_settings()

    last = args.stage or args.until

    def wanted():  # cards.py finds the cards, so this changes once it has run
        return [(s, t) for s, t in targets() if (not args.cards or t in args.cards or t == COMPUTER)
                and (last is None or s == last)]

    force = {item for item in wanted() if item[0] == args.stage}
    if not args.run:
        todo, memo = plan(settings, wanted(), force), {}
        for stage, target in targets():
            state, why = status(stage, target, settings, memo)
            print('%-10s %-11s %-8s %s%s' % (target, stage, state, why,
                                            '  <- would run' if (stage, target) in todo else ''))
        if not cards():
            print('(no cards yet: Cards finds them)')
        return
    ran = set()
    while True:
        todo = [item for item in plan(settings, wanted(), force - ran) if item not in ran]
        if not todo:
            break
        stage, target = todo[0]
        print('== %s: %s' % (target, stage), flush=True)
        for line in command_lines(stage, target, settings):
            if subprocess.call(line, cwd=ROOT):
                raise SystemExit('%s %s failed' % (target, stage))
        stamp(stage, target, settings)
        ran.add((stage, target))
    print('everything asked for is up to date')


if __name__ == '__main__':
    main()
