"""A small reader and writer for KiCad's S-expression files.

A list is a Python list, a bare atom is a Sym and a quoted string is a str, so writing a parsed file
back keeps the quoting. Numbers stay atoms (Sym) on reading; write() also takes int and float.
"""
import re


class Sym(str):
    """A bare atom: a keyword like symbol or a number like 2.54."""


_TOKEN = re.compile(r'\s*(?:(\()|(\))|"((?:[^"\\]|\\.)*)"|([^\s()"]+))')
_ESCAPE = re.compile(r'\\(.)')


def parse(text):
    stack, top = [], []
    for opening, closing, string, atom in _TOKEN.findall(text):
        if opening:
            stack.append(top)
            top = []
        elif closing:
            done, top = top, stack.pop()
            top.append(done)
        elif atom:
            top.append(Sym(atom))
        else:
            top.append(_ESCAPE.sub(r'\1', string) if '\\' in string else string)
    return top[0] if len(top) == 1 else top


def _atom(value):
    if isinstance(value, Sym):
        return str(value)
    if isinstance(value, bool):
        return 'yes' if value else 'no'
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        text = ('%.4f' % value).rstrip('0').rstrip('.')
        return '0' if text == '-0' else text
    return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"') + '"'


def write(expr, indent=0):
    if not isinstance(expr, list):
        return _atom(expr)
    head = [item for item in expr if not isinstance(item, list)]
    body = [item for item in expr if isinstance(item, list)]
    text = '(' + ' '.join(_atom(item) for item in head)
    if not body:
        return text + ')'
    pad = '\t' * (indent + 1)
    return text + ''.join('\n' + pad + write(item, indent + 1) for item in body) + '\n' + '\t' * indent + ')'


def find(expr, key):
    """The first child list whose head is key, or None."""
    return next((item for item in expr if isinstance(item, list) and item and item[0] == key), None)


def find_all(expr, key):
    return [item for item in expr if isinstance(item, list) and item and item[0] == key]
