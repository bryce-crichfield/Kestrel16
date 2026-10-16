"""Where KiCad is installed, and the pcbnew workarounds place.py and route.py share.

kicad.py runs under any Python 3; place.py and route.py run under KiCad's own Python, which has pcbnew. So this
module imports nothing from KiCad at the top, and pcbnew only inside the functions that need it.
"""
import glob
import os
import sys

POWER = ('GND', 'VCC')


def find_kicad(relative):
    """A file in KiCad's install, like 'bin/kicad-cli.exe' or 'share/kicad/symbols', or None. When this is
    KiCad's own Python, its install comes first; otherwise the newest version in the usual places."""
    own = os.path.join(os.path.dirname(sys.executable), '..', relative)
    if os.path.exists(own):
        return os.path.normpath(own)
    patterns = [os.path.join(os.environ.get('ProgramFiles', 'C:/Program Files'), 'KiCad', '*', relative),
                os.path.join('/usr', relative), os.path.join('/usr/share/kicad', relative.replace('share/kicad/', '')),
                os.path.join('/Applications/KiCad/KiCad.app/Contents', relative)]
    for pattern in patterns:
        found = sorted(glob.glob(pattern))
        if found:
            return found[-1]
    return None


def kicad_cli():
    return find_kicad('bin/kicad-cli.exe') or find_kicad('bin/kicad-cli') or 'kicad-cli'


def net_name(nets, name):
    """The key a power net has in a KiCad netlist: 'VCC' when it comes from a power symbol, '/VCC' from a label on
    the root sheet. None if there is no such net."""
    return next((key for key in (name, '/' + name) if key in nets), None)


_app = None


def start_pcbnew():
    """Imports pcbnew, ready for scripting. pcbnew's DSN export raises wx asserts on harmless things (a zone outline
    it thinks is open, for one), and without a wx app an assert stops the process. So there is an app, asserts are
    off, and so is wx's logging, which otherwise reports 'duplicate image handler' a dozen times per board loaded."""
    global _app
    import pcbnew
    import wx
    if _app is None:
        _app = wx.App(False)
        wx.DisableAsserts()
        wx.Log.EnableLogging(False)
    return pcbnew


def fill_zones(path):
    """Fills a saved board's zones in place and returns the board. ZONE_FILLER does nothing on a board built in
    memory, so this reloads it from the file first."""
    pcbnew = start_pcbnew()
    board = pcbnew.LoadBoard(path)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(path)
    return board
