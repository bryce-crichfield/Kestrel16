"""Routes a placed KiCad board with Freerouting, keeps the best candidate, and writes the fabrication files.

Run it with KiCad's own Python after place.py:

  "C:/Program Files/KiCad/9.0/bin/python.exe" Tools/eda/route.py "Build/KiCad/Register 16"

Each placement in Candidates/ goes out as a Specctra DSN, with the inner layers marked as power so Freerouting routes
only the front and back and reaches GND and VCC through the planes. Freerouting routes it headless and writes a
session (SES), which is imported back onto the board. The candidates are scored by unrouted connections, then vias,
then track length; the best becomes <project>.kicad_pcb. KiCad's DRC checks it against the schematic, and the
Gerbers, drill files and a BOM go to Fabrication/.

Freerouting 2.x needs Java 21 or later. The jar is found in KiCad's Freerouting plugin, or pass --freerouting.
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr  # noqa: E402
from kicad_env import fill_zones, kicad_cli, start_pcbnew  # noqa: E402
from sexpr import find, find_all  # noqa: E402

PLUGIN_JARS = os.path.join(os.path.expanduser('~'), 'Documents', 'KiCad', '*', '3rdparty', 'plugins', '*freerouting*',
                           'jar', 'freerouting-*.jar')
COURSIER_JAVAS = os.path.join(os.path.expanduser('~'), 'AppData', 'Local', 'Coursier', 'cache', 'arc', '**', 'bin',
                              'java.exe')
TRACK = 250      # um, trace width
CLEARANCE = 200  # um
GERBER_LAYERS = 'F.Cu,In1.Cu,In2.Cu,B.Cu,F.Mask,B.Mask,F.Silkscreen,B.Silkscreen,Edge.Cuts'


# ---------------------------------------------------------------------------
# Finding the tools

def find_freerouting(path=None):
    jars = [path] if path else sorted(glob.glob(PLUGIN_JARS))
    if not jars or not os.path.exists(jars[-1]):
        raise SystemExit("can't find freerouting.jar; install KiCad's Freerouting plugin or pass --freerouting")
    return jars[-1]


def java_major(java):
    try:
        result = subprocess.run([java, '-version'], capture_output=True, text=True)
    except OSError:
        return 0
    match = re.search(r'version "(\d+)(?:\.(\d+))?', result.stderr)
    if not match:
        return 0
    major = int(match.group(1))
    return int(match.group(2)) if major == 1 else major  # "1.8" is Java 8


def find_java(path=None):
    choices = [path] if path else []
    if os.environ.get('JAVA_HOME'):
        choices.append(os.path.join(os.environ['JAVA_HOME'], 'bin', 'java'))
    choices.append(shutil.which('java') or 'java')
    choices += sorted(glob.glob(COURSIER_JAVAS, recursive=True), reverse=True)  # e.g. cs java-home --jvm temurin:21
    for java in choices:
        if java_major(java) >= 21:
            return java
    raise SystemExit('Freerouting needs Java 21 or later; install one (e.g. cs java-home --jvm temurin:21), '
                     'or pass --java')


# ---------------------------------------------------------------------------
# One candidate

def prepare_dsn(text):
    """Keeps Freerouting off the plane layers and sets the trace width and clearance."""
    text = re.sub(r'(\(layer In\d+\.Cu\s*\(type )signal', r'\1power', text)
    text = re.sub(r'\(width \d+(?:\.\d+)?\)', '(width %d)' % TRACK, text)
    return re.sub(r'\(clearance \d+(?:\.\d+)?\)', '(clearance %d)' % CLEARANCE, text)


def import_ses(board, path):
    """Adds a Freerouting session's wires and vias to the board. (pcbnew.ImportSpecctraSES fails silently here.)"""
    with open(path, encoding='utf-8') as file:
        routes = find(sexpr.parse(file.read()), 'routes')
    resolution = find(routes, 'resolution')
    scale = {'um': 1000, 'mil': 25400, 'mm': 1000000}[resolution[1]] / float(resolution[2])  # file unit -> nm
    layers = {board.GetLayerName(layer): layer for layer in board.GetEnabledLayers().CuStack()}

    def point(x, y):
        return pcbnew.VECTOR2I(int(float(x) * scale), int(-float(y) * scale))

    for net in find_all(find(routes, 'network_out'), 'net'):
        item = board.FindNet(net[1])
        if item is None:
            raise SystemExit('%s routes unknown net %s' % (path, net[1]))
        for wire in find_all(net, 'wire'):
            shape = find(wire, 'path')
            layer, width, coords = layers[shape[1]], int(float(shape[2]) * scale), shape[3:]
            corners = [point(coords[i], coords[i + 1]) for i in range(0, len(coords) - 1, 2)]
            for start, end in zip(corners, corners[1:]):
                track = pcbnew.PCB_TRACK(board)
                track.SetStart(start)
                track.SetEnd(end)
                track.SetWidth(width)
                track.SetLayer(layer)
                track.SetNet(item)
                board.Add(track)
        for via in find_all(net, 'via'):
            size, drill = (int(v) * 1000 for v in re.search(r'_(\d+):(\d+)_um', via[1]).groups())
            item_via = pcbnew.PCB_VIA(board)
            item_via.SetPosition(point(via[2], via[3]))
            item_via.SetWidth(size)
            item_via.SetDrill(drill)
            item_via.SetNet(item)
            board.Add(item_via)


def prune(board):
    """Deletes the dead-end stubs Freerouting leaves behind, until none are left. A stub connects nothing."""
    removed = 0
    while True:
        board.BuildConnectivity()
        connectivity = board.GetConnectivity()
        stubs = [track for track in board.GetTracks()
                 if track.GetClass() == 'PCB_TRACK' and connectivity.TestTrackEndpointDangling(track, False)]
        if not stubs:
            return removed
        for track in stubs:
            board.Remove(track)
        removed += len(stubs)


def score(board):
    """(unrouted connections, vias, track length in mm): lower is better, in that order."""
    board.BuildConnectivity()
    unrouted = board.GetConnectivity().GetUnconnectedCount(True)
    vias = length = 0
    for track in board.GetTracks():
        if track.GetClass() == 'PCB_VIA':
            vias += 1
        else:
            length += pcbnew.ToMM(track.GetLength())
    return unrouted, vias, round(length)


def route(placed, folder, java, jar, passes, threads, timeout):
    """Routes one placed board into folder; returns (score, routed board path)."""
    stem = os.path.join(folder, os.path.splitext(os.path.basename(placed))[0])
    dsn, ses, routed = stem + '.dsn', stem + '.ses', stem + '.kicad_pcb'
    board = pcbnew.LoadBoard(placed)
    if not pcbnew.ExportSpecctraDSN(board, os.path.abspath(dsn)):
        raise SystemExit("can't export %s as DSN" % placed)
    with open(dsn, encoding='utf-8') as file:
        text = prepare_dsn(file.read())
    with open(dsn, 'w', encoding='utf-8') as file:
        file.write(text)
    if os.path.exists(ses):
        os.remove(ses)
    command = [java, '-jar', jar, '-de', os.path.abspath(dsn), '-do', os.path.abspath(ses), '-mp', str(passes),
               '-mt', str(threads), '--gui.enabled=false']
    with open(stem + '.log', 'w', encoding='utf-8') as log:
        try:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired:
            print('  %s: Freerouting ran past %d s and was stopped' % (os.path.basename(placed), timeout))
    if not os.path.exists(ses):
        raise SystemExit('Freerouting wrote no session for %s; see %s.log' % (placed, stem))
    import_ses(board, ses)
    prune(board)
    board.Save(routed)
    return score(fill_zones(routed)), routed


# ---------------------------------------------------------------------------
# Checking and fabrication

def drc(path, cli):
    """KiCad's DRC with schematic parity; returns the report's problem counts."""
    report = os.path.splitext(path)[0] + '-drc.json'
    subprocess.run([cli, 'pcb', 'drc', '--schematic-parity', '--severity-all', '--format', 'json',
                    '-o', report, path], capture_output=True)
    with open(report, encoding='utf-8') as file:
        data = json.load(file)
    counts = {}
    for section in ('violations', 'unconnected_items', 'schematic_parity'):
        for item in data.get(section, []):
            if item.get('severity') != 'ignore':
                key = '%s %s' % (item.get('severity', 'error'), item.get('type', section))
                counts[key] = counts.get(key, 0) + 1
    return counts, report


def fabricate(directory, name, cli):
    out = os.path.join(directory, 'Fabrication')
    os.makedirs(out, exist_ok=True)
    pcb = os.path.join(directory, name + '.kicad_pcb')
    subprocess.run([cli, 'pcb', 'export', 'gerbers', '-l', GERBER_LAYERS, '-o', out + os.sep, pcb],
                   check=True, capture_output=True)
    subprocess.run([cli, 'pcb', 'export', 'drill', '--generate-map', '--map-format', 'gerberx2',
                    '-o', out + os.sep, pcb], check=True, capture_output=True)
    subprocess.run([cli, 'sch', 'export', 'bom', '--fields', 'Reference,Value,Footprint,${QUANTITY}',
                    '--labels', 'Refs,Value,Footprint,Qty', '--group-by', 'Value,Footprint',
                    '-o', os.path.join(out, name + '-bom.csv'), os.path.join(directory, name + '.kicad_sch')],
                   check=True, capture_output=True)
    return out


# ---------------------------------------------------------------------------

def route_project(directory, java=None, jar=None, passes=100, threads=4, timeout=1200, cli=None):
    """Routes every candidate, keeps the best as <project>.kicad_pcb, checks it and writes Fabrication/. Returns
    True when the board is fully routed and DRC finds no errors."""
    start_pcbnew()
    cli = cli or kicad_cli()
    name = os.path.basename(os.path.normpath(directory))
    placed = sorted(glob.glob(os.path.join(directory, 'Candidates', '*.kicad_pcb')),
                    key=lambda path: int(re.sub(r'\D', '', os.path.basename(path)) or 0))
    placed = placed or [os.path.join(directory, name + '.kicad_pcb')]
    java, jar = find_java(java), find_freerouting(jar)
    folder = os.path.join(directory, 'Routing')
    os.makedirs(folder, exist_ok=True)
    results = []
    for path in placed:
        result, routed = route(path, folder, java, jar, passes, threads, timeout)
        results.append((result, routed))
        print('  %s: %d unrouted, %d vias, %d mm of track' % ((os.path.basename(path),) + result))
    (unrouted, vias, length), best = min(results)
    pcb = os.path.join(directory, name + '.kicad_pcb')
    shutil.copyfile(best, pcb)
    print('%s: best is %s -> %s' % (name, os.path.basename(best), pcb))
    counts, report = drc(pcb, cli)
    print('  DRC: %s (%s)' % (', '.join('%d %s' % (n, key) for key, n in sorted(counts.items())) or 'clean', report))
    if unrouted:
        print('  %d connections are still unrouted; not writing fabrication files' % unrouted)
        return False
    print('  -> %s' % fabricate(directory, name, cli))
    return not any(key.startswith('error') for key in counts)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('project', help='a placed project directory, e.g. "Build/KiCad/Register 16"')
    parser.add_argument('--java', help="a Java 21+ executable (default: JAVA_HOME, PATH, then Coursier's cache)")
    parser.add_argument('--freerouting', help="freerouting.jar (default: KiCad's Freerouting plugin)")
    parser.add_argument('--passes', type=int, default=100, help='most routing passes per candidate (default 100)')
    parser.add_argument('--threads', type=int, default=4, help='Freerouting optimizer threads (default 4)')
    parser.add_argument('--timeout', type=int, default=1200, help='seconds per candidate (default 1200)')
    parser.add_argument('--kicad-cli', help="for DRC and the fabrication files (default: KiCad's own)")
    args = parser.parse_args()
    ok = route_project(args.project, args.java, args.freerouting, args.passes, args.threads, args.timeout,
                       args.kicad_cli)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
