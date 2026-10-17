"""Build printable datasheets from component YAML files.

    python build.py                  # every components/*.yaml
    python build.py register16       # just one

Writes out/<name>.html and out/<name>.pdf (Letter).
"""
import re
import shutil
import sys
from pathlib import Path

import yaml
from jinja2 import Environment, FileSystemLoader
from markupsafe import Markup, escape

import svg

ROOT = Path(__file__).parent
OUT = ROOT / 'out'


def sym(s):  # t_PD -> t<sub>PD</sub>
    return Markup(re.sub(r'\bt_(\w+)', r't<sub>\1</sub>', str(escape(s))))


def signals(s):  # keep active-low names like /LOAD from wrapping after the slash
    return Markup(re.sub(r'(/[A-Z][\w\[\]:]*)', r'<span class="nobr">\1</span>', str(escape(s))))


def build(path):
    c = yaml.safe_load(path.read_text(encoding='utf-8'))
    env = Environment(loader=FileSystemLoader(ROOT / 'templates'), autoescape=True)
    env.filters['sym'] = sym
    env.filters['signals'] = signals
    html = env.get_template('datasheet.html.j2').render(
        c=c, pinout_svg=svg.pinout(c['name'], c['pins']), schematic_svg=svg.schematic(c['schematic']))
    OUT.mkdir(exist_ok=True)
    shutil.copy(ROOT / 'templates' / 'style.css', OUT / 'style.css')
    out = OUT / f'{path.stem}.html'
    out.write_text(html, encoding='utf-8')
    try:
        from weasyprint import HTML
        HTML(out).write_pdf(OUT / f'{path.stem}.pdf')
    except (ImportError, OSError) as e:  # OSError: WeasyPrint's GTK libraries missing (common on Windows)
        print(f'  PDF skipped ({type(e).__name__}): open the .html in a browser and print to PDF')
    print(f'built {path.stem}')


if __name__ == '__main__':
    names = sys.argv[1:] or [p.stem for p in sorted((ROOT / 'components').glob('*.yaml'))]
    for n in names:
        build(ROOT / 'components' / f'{n}.yaml')
