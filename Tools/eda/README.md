# Tools/eda: from components to a board

These scripts take a component from `Components/` to a manufacturable PCB: a KiCad schematic, a placed board, a
routed board, and Gerbers. Each stage checks its own output before the next one starts.

```
Components/<name>.yaml ──kicad.py──► schematic ──place.py──► placed boards ──route.py──► routed board + Fabrication/
                         (--check:            (anneal into a          (Freerouting each candidate,
                          netlist round trip,  slot grid, caps,        keep the best, DRC against
                          ERC)                 headers, planes)        the schematic, Gerbers, BOM)
```

## Running it

```sh
python Tools/eda/kicad.py "Register 16" --check
"C:/Program Files/KiCad/9.0/bin/python.exe" Tools/eda/place.py "Build/KiCad/Register 16"
"C:/Program Files/KiCad/9.0/bin/python.exe" Tools/eda/route.py "Build/KiCad/Register 16"
```

`kicad.py` runs under any Python 3 with PyYAML. `place.py` and `route.py` need `pcbnew`, which only KiCad's own Python
has. `route.py` also needs Java 21 or later and KiCad's Freerouting plugin, which provides the jar. Any recent JDK
works, for example `cs java-home --jvm temurin:21`. The script looks for Java in `--java`, then `JAVA_HOME`, then
`PATH`, then Coursier's cache.

For Register 16, placement takes under a minute. Routing takes about 30 s for each of the 5 candidates.

## The files

| File | Role |
|---|---|
| `netlist.py` | Reads `Components/*.yaml` into nets, naming pins the way the simulator does |
| `parts.yaml` | Maps each primitive to a real part: symbol, footprint, pin numbers, power pins, and ties for unused inputs |
| `kicad.py` | Writes the schematic, one sheet per component. `--check` proves it matches the netlist |
| `place.py` | Anneals the chips into a grid, adds caps and headers, and writes 4-layer boards |
| `route.py` | Routes the candidates with Freerouting, scores them, runs DRC, and writes the fabrication files |
| `kicad_env.py` | Finds KiCad, and holds the pcbnew workarounds below |
| `sexpr.py` | Reads and writes KiCad's S-expression files |

A project directory, such as `Build/KiCad/Register 16/`, ends up holding:

| Path | Written by |
|---|---|
| `*.kicad_sch`, `*.kicad_pro`, `*.pins.json` | `kicad.py` (`pins.json` maps `U12:3` back to a logical pin, for `--check`) |
| `<name>.net`, `<name>.erc.json` | `kicad.py --check`. `place.py` re-exports the netlist on every run |
| `Candidates/1..N.kicad_pcb` | `place.py`, best placement first |
| `Routing/N.dsn`, `.ses`, `.log`, `.kicad_pcb` | `route.py`, one set per candidate. The `.log` is Freerouting's |
| `<name>.kicad_pcb` | `place.py` writes the best placement here. `route.py` replaces it with the best routed board |
| `<name>-drc.json` | `route.py` |
| `Fabrication/` | `route.py`: Gerbers, drill files and map, and `<name>-bom.csv` |

## How each stage works

**Schematic (`kicad.py`).** Nothing is wired with lines. Every pin gets a label naming its net, and pins with the same
label connect. Gates of one kind are packed into packages one unit at a time. Leftover units are tied off, and each
package gets a 100nF cap. `--check` exports the netlist with `kicad-cli`, maps it back through `pins.json`, and
compares it net for net with the flattened source.

**Placement (`place.py`).** The board is a grid of slots, about 1.4 per chip. Every DIP lies on its side with pin 1
at the bottom left. Simulated annealing swaps chips between slots to minimize the half-perimeter wirelength of the
signal nets. GND and VCC are left out, since they are planes. Several independent anneals run, and the best few
become candidates. Headers and the bulk cap sit fixed along the bottom edge, so their pins pull connected chips
toward them. Each decoupling cap goes just past its chip's VCC pin. The boards have 4 layers: signals on F.Cu and
B.Cu, In1 a GND plane and In2 a VCC plane.

**Routing (`route.py`).** Each candidate is exported as a Specctra DSN, with the inner layers marked `power` so
Freerouting routes only the outer two. Freerouting writes a session (SES). It is imported back onto the board, and
dead-end stubs are pruned. Candidates are ranked by unrouted connections, then vias, then track length. The winner is
DRC-checked against the schematic. Fabrication files are written only when nothing is unrouted.

## Quirks worked around

Knowing these saves rediscovering them:

- **pcbnew asserts kill the process.** `ExportSpecctraDSN` hits wx asserts on harmless things. With no wx app, an
  assert is a fatal breakpoint (exit 0x80000003). `kicad_env.start_pcbnew()` creates an app, turns asserts off, and
  silences wx's log spam.
- **Paths must be native.** `ExportSpecctraDSN` returns False when given a Git Bash path like `/c/...`. Always pass
  `os.path.abspath`.
- **Zones only fill on a loaded board.** `ZONE_FILLER` does nothing to a board built in memory. `fill_zones()` saves,
  reloads, fills, and saves again.
- **Zone outlines must be closed explicitly** (`SetClosed(True)`). Otherwise the DSN export asserts.
- **`ImportSpecctraSES` fails silently** from Python (it returns False). `route.py` has its own importer, about 30
  lines of S-expression parsing.
- **Freerouting leaves stubs.** Dead-end track segments show up as DRC `track_dangling` warnings. `prune()` removes
  them until none are left.
- **KiCad exports the plane layers as `signal`.** `prepare_dsn()` rewrites them to `power`. Freerouting then reaches
  GND and VCC through vias into the planes, rather than routing them as tracks.
- **Power nets are `VCC` or `/VCC`.** The name depends on whether the net comes from a power symbol or a root-sheet
  label. `kicad_env.net_name()` looks up whichever exists.
- **`board.GetLayerName()` can report a zone's layer wrongly** (it showed F.Cu for In1). Check the file before
  trusting it.

## Tuning

| Knob | Where | Effect |
|---|---|---|
| `--density` | `place.py` | Slots per chip. More slots spread the board out and leave more routing room |
| `--runs`, `--moves` | `place.py` | More anneals, or longer ones, give better placements and take longer |
| `--candidates` | `place.py` | How many placements `route.py` routes. Routing time grows linearly with it |
| `--passes`, `--threads` | `route.py` | Freerouting's pass limit and its thread count |
| `TRACK`, `CLEARANCE` | `route.py` | 250 µm traces with 200 µm clearance. Any cheap fab can make these |

Freerouting is nondeterministic (it runs multithreaded), so the scores change slightly between runs. Most of its
time goes into the first routing pass, which already saturates the CPU. Running several candidates at once would not
help much, and each Freerouting process can grow past 10 GB of memory. To route faster, route fewer candidates.
