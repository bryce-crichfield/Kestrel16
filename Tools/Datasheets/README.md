# Datasheets

One YAML file per component in `components/`; one template for all of them.

    pip install pyyaml jinja2 weasyprint
    python build.py              # all components
    python build.py register16   # one

Output goes to `out/<name>.html` and `out/<name>.pdf` (US Letter).

## Component file

| Key | Contents |
|---|---|
| `name`, `id`, `revision` | Title block |
| `summary` | One-paragraph description at the top |
| `specs` | `[label, value]` pairs, shown 3 per row |
| `pins` | `{name, dir: in/out, width, active: low, clock: true, desc}` — drives the pinout symbol and pin table |
| `schematic` | See below |
| `tables` | Transition tables: `{title, legend, columns, rows}` |
| `bom` | `[ref, qty, part, description, package]` |

## Schematic

Chips are placed in left-to-right `columns`; every wire is drawn with right angles.

    schematic:
      columns: [[U1, U2], [U3, U4]]
      chips:
        U1: {part: 74HC377, label: bits 7–0,
             inputs: [{name: D, width: 8}, {name: /E, active: low}, {name: CP, clock: true}],
             outputs: [{name: Q, width: 8}]}
      nets:
        - {from: "D[7:0]", to: [U1.D], width: 8}   # bare name = component port (flag)
        - {from: U1.Q, to: [U3.A, U4.A], width: 8}  # Chip.pin

Rules: a net's chip destinations must all be in one column. A component input that feeds a
later column is routed along the bottom. Output ports must come from the last column.
Quote any YAML value containing a comma.
