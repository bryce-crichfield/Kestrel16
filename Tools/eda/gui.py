"""A window on the EDA pipeline: every stage of every card, whether it's up to date, and a click to bring it there.

  py Tools/eda/gui.py

The grid has a row for the computer (Generate, Cards, Floorplan) and one per card (Layout, Congestion, Route). A
cell is green when up to date, amber when stale (something it needs changed since, or its settings did), grey when
it hasn't run and red when it failed; hover it to see why. Clicking a cell runs it, after whatever it needs that
isn't up to date; Build all runs everything that isn't. Each column's Settings button sets that stage's options.
The stages, settings and stamps are build.py's, so the window and the command line agree on what's stale.
"""
import os
import queue
import subprocess
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import build

COLOURS = {'done': '#9be29b', 'stale': '#f5c66b', 'missing': '#d9d9d9', 'failed': '#f08a8a', 'running': '#8ec5f5',
           'queued': '#cfe4f7'}
TEXT = {'done': 'done', 'stale': 'stale', 'missing': 'not run', 'failed': 'failed', 'running': 'running',
        'queued': 'queued'}


class App:
    def __init__(self, root):
        self.root = root
        root.title('Kestrel16 EDA')
        self.settings = build.load_settings()
        self.process, self.lines, self.queue = None, queue.Queue(), []  # queue: [(stage, target, [lines])]
        self.current, self.failed, self.cells, self.all = None, set(), {}, False

        top = ttk.Frame(root, padding=8)
        top.pack(fill='x')
        ttk.Label(top, text="KiCad's Python").grid(row=0, column=0, sticky='w')
        self.kicad = tk.StringVar(value=build.kicad_python(self.settings))
        ttk.Entry(top, textvariable=self.kicad, width=60).grid(row=0, column=1, sticky='we', padx=4)
        ttk.Button(top, text='Browse', command=lambda: self.browse(self.kicad)).grid(row=0, column=2)
        ttk.Label(top, text='Parts').grid(row=1, column=0, sticky='w', pady=(4, 0))
        self.parts = tk.StringVar(value=os.path.basename(build.parts_file(self.settings)))
        picker = ttk.Combobox(top, textvariable=self.parts, values=build.parts_files(), state='readonly', width=30)
        picker.grid(row=1, column=1, sticky='w', padx=4, pady=(4, 0))
        picker.bind('<<ComboboxSelected>>', lambda _: (self.save(), self.paint()))
        top.columnconfigure(1, weight=1)

        self.grid = ttk.LabelFrame(root, text='Pipeline', padding=8)
        self.grid.pack(fill='x', padx=8)
        buttons = ttk.Frame(root, padding=(8, 6))
        buttons.pack(fill='x')
        ttk.Button(buttons, text='Build all', command=self.build_all).pack(side='left')
        ttk.Button(buttons, text='Refresh', command=self.draw).pack(side='left', padx=6)
        self.stop_button = ttk.Button(buttons, text='Stop', command=self.stop, state='disabled')
        self.stop_button.pack(side='left')

        self.log = tk.Text(root, height=24, width=110, wrap='none', font=('Consolas', 9))
        self.log.pack(fill='both', expand=True, padx=8, pady=(0, 8))
        self.status = tk.StringVar(value='ready')
        ttk.Label(root, textvariable=self.status, padding=(8, 0, 8, 6)).pack(anchor='w')
        self.draw()
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.drain)

    # ------------------------------------------------------------------ the grid

    def draw(self):
        """Lays the grid out again: cards.py may have found new cards."""
        for child in self.grid.winfo_children():
            child.destroy()
        self.cells = {}
        for column, stage in enumerate(build.STAGES, start=1):
            ttk.Label(self.grid, text=stage.name, anchor='center').grid(row=0, column=column, sticky='we', padx=2)
            if stage.options:
                ttk.Button(self.grid, text='Settings', width=9,
                           command=lambda s=stage: self.configure(s)).grid(row=1, column=column, padx=2, pady=(0, 4))
        rows = [build.COMPUTER] + build.cards()
        for row, target in enumerate(rows, start=2):
            ttk.Label(self.grid, text=target).grid(row=row, column=0, sticky='w', padx=(0, 8))
            for column, stage in enumerate(build.STAGES, start=1):
                if stage.card == (target == build.COMPUTER):
                    continue
                cell = tk.Button(self.grid, width=11, relief='groove',
                                 command=lambda s=stage.name, t=target: self.run_cell(s, t))
                cell.grid(row=row, column=column, padx=2, pady=1, sticky='we')
                cell.bind('<Enter>', lambda _, s=stage.name, t=target: self.explain(s, t))
                cell.bind('<Leave>', lambda _: self.status.set(self.state_line()))
                self.cells[(stage.name, target)] = cell
            if target != build.COMPUTER:
                ttk.Button(self.grid, text='Open', width=6,
                           command=lambda t=target: self.open_folder(t)).grid(row=row, column=len(build.STAGES) + 1,
                                                                              padx=(8, 0))
        if len(rows) == 1:
            ttk.Label(self.grid, text='Run Cards to find the cards').grid(row=3, column=0, columnspan=4, sticky='w')
        self.paint()

    def state(self, stage, target, memo):
        item = (stage, target)
        if item == (self.current or (None, None))[:2]:
            return 'running', 'running now'
        if any(item == queued[:2] for queued in self.queue):
            return 'queued', 'waiting its turn'
        if item in self.failed:
            return 'failed', 'its last run failed: see the log'
        return build.status(stage, target, self.settings, memo)

    def paint(self):
        memo = {}
        for (stage, target), cell in self.cells.items():
            state, _ = self.state(stage, target, memo)
            cell.configure(text=TEXT[state], bg=COLOURS[state], activebackground=COLOURS[state])

    def explain(self, stage, target):
        self.status.set('%s %s: %s' % (target, stage, self.state(stage, target, {})[1]))

    def state_line(self):
        return 'running %s %s' % (self.current[1], self.current[0]) if self.current else 'ready'

    # ------------------------------------------------------------------ settings

    def save(self):
        self.settings['kicad_python'] = self.kicad.get()
        self.settings['parts'] = self.parts.get()
        build.save_settings(self.settings)

    def configure(self, stage):
        dialog = tk.Toplevel(self.root)
        dialog.title('%s settings' % stage.name)
        dialog.transient(self.root)
        frame = ttk.Frame(dialog, padding=10)
        frame.pack(fill='both')
        values = {}
        for row, (flag, text, kind, default) in enumerate(stage.options):
            value = build.option(self.settings, stage.name, flag, default)
            ttk.Label(frame, text=text).grid(row=row, column=0, sticky='w', pady=2)
            if kind is bool:
                values[flag] = tk.BooleanVar(value=bool(value))
                ttk.Checkbutton(frame, variable=values[flag]).grid(row=row, column=1, sticky='w')
            else:
                values[flag] = tk.StringVar(value='' if value is None else str(value))
                ttk.Entry(frame, textvariable=values[flag], width=44).grid(row=row, column=1, sticky='we', padx=4)
                if kind == 'file':
                    ttk.Button(frame, text='...', width=3,
                               command=lambda v=values[flag]: self.browse(v)).grid(row=row, column=2)

        def ok():
            chosen = {}
            for flag, text, kind, _ in stage.options:
                value = values[flag].get()
                if kind is int and value.strip():
                    try:
                        value = int(value)
                    except ValueError:
                        messagebox.showerror('Settings', '%s must be a whole number' % text, parent=dialog)
                        return
                chosen[flag] = value
            self.settings.setdefault('stages', {})[stage.name] = chosen
            self.save()
            dialog.destroy()
            self.paint()  # a changed setting makes the stage stale

        buttons = ttk.Frame(frame)
        buttons.grid(row=len(stage.options), column=0, columnspan=3, pady=(8, 0), sticky='e')
        ttk.Button(buttons, text='Defaults', command=lambda: [
            values[f].set(d if k is bool else str(d)) for f, _, k, d in stage.options]).pack(side='left', padx=4)
        ttk.Button(buttons, text='OK', command=ok).pack(side='left', padx=4)
        ttk.Button(buttons, text='Cancel', command=dialog.destroy).pack(side='left')
        dialog.grab_set()

    def browse(self, variable):
        types = [('KiCad board', '*.kicad_pcb'), ('All files', '*.*')] if variable.get().endswith('.kicad_pcb') else []
        path = filedialog.askopenfilename(initialdir=os.path.dirname(variable.get()) or build.ROOT, filetypes=types)
        if path:
            variable.set(os.path.normpath(path))  # Tk returns forward slashes

    # ------------------------------------------------------------------ running

    def run_cell(self, stage, target):
        """The stage, even if up to date, after whatever it needs that isn't."""
        self.start(build.plan(self.settings, [(stage, target)], {(stage, target)}))

    def build_all(self):
        self.start(build.plan(self.settings), everything=True)

    def start(self, items, everything=False):
        if self.process or self.current:
            return
        self.all = everything
        self.save()
        if not items:
            self.status.set('everything is up to date')
            return
        self.queue = [(stage, target, build.command_lines(stage, target, self.settings)) for stage, target in items]
        self.failed -= {item[:2] for item in self.queue}
        self.log.delete('1.0', 'end')
        self.next()

    def next(self):
        """The next command line: of the current stage, else the next stage's first."""
        if self.current and not self.current[2]:  # its last line succeeded
            stage, target, _ = self.current
            build.stamp(stage, target, self.settings)
            self.current = None
            if stage == 'Cards':  # it finds the cards: show them, and Build all plans again to take them in
                self.draw()
                if self.all:
                    self.queue = [(s, t, build.command_lines(s, t, self.settings))
                                  for s, t in build.plan(self.settings)]
        if self.current is None:
            if not self.queue:
                self.finish('done')
                return
            stage, target, lines = self.queue.pop(0)
            self.current = (stage, target, list(lines))
            self.write('== %s: %s\n' % (target, stage))
        line = self.current[2].pop(0)
        self.write('> %s\n' % subprocess.list2cmdline(line))
        self.status.set(self.state_line())
        self.stop_button['state'] = 'normal'
        self.paint()
        try:
            self.process = subprocess.Popen(line, cwd=build.ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            text=True, bufsize=1, encoding='utf-8', errors='replace',
                                            env=dict(os.environ, PYTHONUNBUFFERED='1'),  # lines as they're printed
                                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except OSError as error:
            self.write('%s\n' % error)
            self.fail()
            return
        threading.Thread(target=self.read, args=(self.process,), daemon=True).start()

    def read(self, process):
        for text in process.stdout:
            self.lines.put(text)
        self.lines.put((process.wait(),))

    def drain(self):
        while not self.lines.empty():
            item = self.lines.get()
            if isinstance(item, tuple):
                self.process = None
                if item[0]:
                    self.write('exit code %d\n' % item[0])
                    self.fail()
                else:
                    self.next()
            else:
                self.write(item)
        self.root.after(100, self.drain)

    def fail(self):
        if self.current:
            self.failed.add(self.current[:2])
        self.current, self.queue = None, []
        self.finish('failed')

    def write(self, text):
        self.log.insert('end', text)
        self.log.see('end')

    def finish(self, status):
        self.status.set(status)
        self.stop_button['state'] = 'disabled'
        self.paint()

    def stop(self):
        self.queue = []
        if self.process:
            if os.name == 'nt':  # the whole tree, so route.py's Freerouting doesn't run on
                subprocess.run(['taskkill', '/T', '/F', '/PID', str(self.process.pid)], capture_output=True,
                               creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            else:
                self.process.kill()
            self.write('stopped\n')

    def open_folder(self, card):
        folder = build.project(card)
        if not os.path.isdir(folder):
            messagebox.showinfo('EDA', '%s has no project yet: run its Layout first' % card)
            return
        os.startfile(folder) if hasattr(os, 'startfile') else subprocess.Popen(['xdg-open', folder])

    def close(self):
        self.stop()
        self.save()
        self.root.destroy()


if __name__ == '__main__':
    root = tk.Tk()
    App(root)
    root.mainloop()
