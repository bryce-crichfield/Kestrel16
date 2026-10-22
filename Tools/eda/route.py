"""Routes a placed KiCad board with Freerouting, keeps the best candidate, and writes the fabrication files.

Run it with KiCad's own Python after place.py:

  "C:/Program Files/KiCad/9.0/bin/python.exe" Tools/eda/route.py "Build/KiCad/Register 16"

Each placement in Candidates/ goes out as a Specctra DSN, with the inner layers that hold a plane marked as power so
Freerouting routes the rest and reaches GND (and VCC, unless place.py --signal-layers 3 left In2 for signals)
through the planes. Track widths, clearances and vias are the project's net classes (Default, Clock, Power; see
kicad_env.NET_CLASSES), which kicad.py wrote into its .kicad_pro. With In2 a signal layer, VCC is routed as Power
tracks and then In2's free space is poured with VCC. Freerouting routes it headless and writes a
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
import threading
import time

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sexpr  # noqa: E402
from kicad_env import fill_zones, kicad_cli, start_pcbnew  # noqa: E402
from sexpr import find, find_all  # noqa: E402

PLUGIN_JARS = os.path.join(os.path.expanduser('~'), 'Documents', 'KiCad', '*', '3rdparty', 'plugins', '*freerouting*',
                           'jar', 'freerouting-*.jar')
COURSIER_JAVAS = os.path.join(os.path.expanduser('~'), 'AppData', 'Local', 'Coursier', 'cache', 'arc', '**', 'bin',
                              'java.exe')
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
    """Keeps Freerouting off the layers that hold a plane. The net classes' rules stay as KiCad wrote them, but
    under their own names: KiCad calls a class a net falls in by pattern "Clock,Default"."""
    for layer in set(re.findall(r'\(plane \S+ \(polygon (\S+)', text)):
        text = re.sub(r'(\(layer %s\s*\(type )signal' % re.escape(layer), r'\1power', text)
    return re.sub(r'\(class (\S+?),Default ', r'(class \1 ', text)


def with_net_classes(board_path, project):
    """Puts the project's .kicad_pro beside a board that has none, as <board>.kicad_pro, so pcbnew loads the board
    with its net classes."""
    own = os.path.splitext(board_path)[0] + '.kicad_pro'
    if os.path.abspath(own) == os.path.abspath(project) or not os.path.isfile(project):
        return
    with open(project, encoding='utf-8') as file:
        settings = json.load(file)
    settings.setdefault('meta', {})['filename'] = os.path.basename(own)
    with open(own, 'w', encoding='utf-8', newline='\n') as file:
        json.dump(settings, file, indent=2)


def pour_vcc(board):
    """A VCC pour over In2 when it's a signal layer (no plane on it), to fill what the tracks leave."""
    if any(z.GetLayer() == pcbnew.In2_Cu for z in board.Zones() if not z.GetIsRuleArea()):
        return
    net = board.FindNet('VCC') or board.FindNet('/VCC')
    if net is None:
        return
    box = board.GetBoardEdgesBoundingBox()
    zone = pcbnew.ZONE(board)
    zone.SetLayer(pcbnew.In2_Cu)
    zone.SetNet(net)
    zone.SetLocalClearance(pcbnew.FromMM(0.3))
    outline = zone.Outline()
    outline.NewOutline()
    for x, y in ((box.GetLeft(), box.GetTop()), (box.GetRight(), box.GetTop()), (box.GetRight(), box.GetBottom()),
                 (box.GetLeft(), box.GetBottom())):
        outline.Append(x, y)
    outline.Outline(0).SetClosed(True)
    board.Add(zone)


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


PASS = re.compile(r'pass #(\d+) .*?\((\d+) unrouted\)')  # Freerouting's line for a finished pass


def route(placed, folder, java, jar, passes, threads, timeout, project):
    """Routes one placed board into folder; returns (score, routed board path), or (None, None) if Freerouting
    stopped without a session."""
    stem = os.path.join(folder, os.path.splitext(os.path.basename(placed))[0])
    dsn, ses, routed = stem + '.dsn', stem + '.ses', stem + '.kicad_pcb'
    with_net_classes(placed, project)
    with_net_classes(routed, project)
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
    # Freerouting's log goes to <candidate>.log, and each finished pass is reported as it comes, so a long route
    # shows how it's going. It writes the session only when it stops by itself, so the timeout loses the candidate.
    start, timer = time.time(), None
    with open(stem + '.log', 'w', encoding='utf-8') as log:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                   encoding='utf-8', errors='replace')
        timer = threading.Timer(timeout, process.kill)
        timer.start()
        for line in process.stdout:
            log.write(line)
            found = PASS.search(line)
            if found:
                print('    pass %s: %s unrouted (%.0f s)' % (found.group(1), found.group(2), time.time() - start),
                      flush=True)
        process.wait()
        timer.cancel()
    if not os.path.exists(ses):
        print('  %s: Freerouting %s, so it wrote no session; see %s.log'
              % (os.path.basename(placed), 'ran past %d s and was stopped' % timeout
                 if time.time() - start >= timeout else 'failed', stem), flush=True)
        return None, None
    import_ses(board, ses)
    prune(board)
    pour_vcc(board)
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

def route_project(directory, java=None, jar=None, passes=25, threads=4, timeout=600, cli=None, candidates=1):
    """Routes the first few candidates (place.py writes them best first), keeps the best as <project>.kicad_pcb,
    checks it and writes Fabrication/. Returns True when the board is fully routed and DRC finds no errors."""
    start_pcbnew()
    cli = cli or kicad_cli()
    name = os.path.basename(os.path.normpath(directory))
    placed = sorted(glob.glob(os.path.join(directory, 'Candidates', '*.kicad_pcb')),
                    key=lambda path: int(re.sub(r'\D', '', os.path.basename(path)) or 0))
    placed = (placed or [os.path.join(directory, name + '.kicad_pcb')])[:candidates]
    java, jar = find_java(java), find_freerouting(jar)
    folder = os.path.join(directory, 'Routing')
    os.makedirs(folder, exist_ok=True)
    results = []
    for k, path in enumerate(placed):
        print('  routing %s (%d of %d)' % (os.path.basename(path), k + 1, len(placed)), flush=True)
        result, routed = route(path, folder, java, jar, passes, threads, timeout,
                               os.path.join(directory, name + '.kicad_pro'))
        if result is None:
            continue
        results.append((result, routed))
        print('  %s: %d unrouted, %d vias, %d mm of track' % ((os.path.basename(path),) + result), flush=True)
    if not results:
        print('%s: no candidate was routed' % name)
        return False
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
    parser.add_argument('--passes', type=int, default=25, help='most routing passes per candidate; Freerouting '
                        'stops sooner once everything is routed (default 25)')
    parser.add_argument('--threads', type=int, default=4, help='Freerouting optimizer threads (default 4)')
    parser.add_argument('--timeout', type=int, default=600, help='seconds per candidate; one stopped by it is '
                        'lost, since Freerouting writes nothing until it finishes (default 600)')
    parser.add_argument('--candidates', type=int, default=1, help="how many of place.py's candidates to route, "
                        'best first (default 1)')
    parser.add_argument('--kicad-cli', help="for DRC and the fabrication files (default: KiCad's own)")
    args = parser.parse_args()
    ok = route_project(args.project, args.java, args.freerouting, args.passes, args.threads, args.timeout,
                       args.kicad_cli, args.candidates)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
