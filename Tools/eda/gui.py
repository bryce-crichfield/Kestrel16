"""A small window for running the EDA tools on a component, one stage at a time or all in a row.

  py Tools/eda/gui.py

Pick a component, then run a stage. Each stage's Settings button sets its command-line options; they are saved in
Build/eda_gui.json. kicad.py and cards.py run under this Python; template.py, place.py and route.py under KiCad's,
which is set in the window too. Output streams into the log, and Stop kills the running tool.
"""
import json
import os
import queue
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
COMPONENTS = os.path.join(ROOT, 'Build', 'Components')
SETTINGS = os.path.join(ROOT, 'Build', 'eda_gui.json')
TEMPLATE = os.path.join(HERE, 'templates', 'Eurocard 3U.kicad_pcb')


def kicad_python():
    sys.path.insert(0, HERE)
    from kicad_env import find_kicad
    return find_kicad('bin/python.exe') or find_kicad('bin/python3') or 'python'


# A stage: its label, the script, which Python runs it, what it takes as its argument (the component's name, its
# KiCad project directory, or nothing), and its options as (flag, label, kind, default). A bool is a plain flag.
STAGES = [
    ('Generate design', None, 'python', None, []),
    ('Schematic', 'kicad.py', 'python', 'component', [('--check', 'Round trip and ERC (needs kicad-cli)', bool, True)]),
    ('Cards report', 'cards.py', 'python', 'component', [
        ('--reserved', 'Pins kept for power', int, 16),
        ('--show', 'Widest nets to list', int, 15)]),
    ('Eurocard template', 'template.py', 'kicad', None, []),
    ('Place', 'place.py', 'kicad', 'project', [
        ('--template', 'Template board (blank: size to fit)', 'file', TEMPLATE),
        ('--gap', 'Slot room past each chip WxH mm', str, '7x4'),
        ('--density', 'Slots per chip (no template)', str, ''),
        ('--runs', 'Anneals', int, 6),
        ('--candidates', 'Placements kept for routing', int, 5),
        ('--moves', 'Moves per anneal (blank: 4000 per chip)', str, ''),
        ('--seed', 'Seed', int, 1)]),
    ('Route', 'route.py', 'kicad', 'project', [
        ('--passes', 'Routing passes per candidate', int, 100),
        ('--threads', 'Freerouting threads', int, 4),
        ('--timeout', 'Seconds per candidate', int, 1200),
        ('--java', 'Java 21+ (blank: find it)', 'file', '')]),
]
GENERATORS = ['make_registers.py', 'make_alu.py', 'make_cpu.py']  # in order: later ones use earlier parts
PIPELINE = ['Schematic', 'Place', 'Route']


class App:
    def __init__(self, root):
        self.root = root
        root.title('Kestrel16 EDA')
        self.settings = self.load()
        self.process, self.lines, self.queue = None, queue.Queue(), []

        top = ttk.Frame(root, padding=8)
        top.pack(fill='x')
        ttk.Label(top, text='Component').grid(row=0, column=0, sticky='w')
        self.component = tk.StringVar(value=self.settings.get('component', 'Register 16'))
        self.choice = ttk.Combobox(top, textvariable=self.component, width=36)
        self.choice.grid(row=0, column=1, sticky='we', padx=4)
        ttk.Button(top, text='Refresh', command=self.refresh).grid(row=0, column=2)
        ttk.Label(top, text="KiCad's Python").grid(row=1, column=0, sticky='w', pady=(4, 0))
        self.kicad = tk.StringVar(value=self.settings.get('kicad_python') or kicad_python())
        ttk.Entry(top, textvariable=self.kicad, width=40).grid(row=1, column=1, sticky='we', padx=4, pady=(4, 0))
        ttk.Button(top, text='Browse', command=lambda: self.browse(self.kicad)).grid(row=1, column=2, pady=(4, 0))
        top.columnconfigure(1, weight=1)

        stages = ttk.LabelFrame(root, text='Stages', padding=8)
        stages.pack(fill='x', padx=8)
        for row, (label, _, _, _, options) in enumerate(STAGES):
            ttk.Button(stages, text=label, width=20, command=lambda l=label: self.run([l])).grid(row=row, column=0, pady=1)
            if options:
                ttk.Button(stages, text='Settings', command=lambda l=label: self.configure(l)).grid(row=row, column=1, padx=4)
        ttk.Button(stages, text='Schematic, Place, Route', command=lambda: self.run(PIPELINE)).grid(row=0, column=2, padx=12)
        ttk.Button(stages, text='Open project folder', command=self.open_folder).grid(row=1, column=2, padx=12)
        self.stop_button = ttk.Button(stages, text='Stop', command=self.stop, state='disabled')
        self.stop_button.grid(row=2, column=2, padx=12)

        self.log = tk.Text(root, height=24, width=110, wrap='none', font=('Consolas', 9))
        self.log.pack(fill='both', expand=True, padx=8, pady=8)
        self.status = tk.StringVar(value='ready')
        ttk.Label(root, textvariable=self.status, padding=(8, 0, 8, 6)).pack(anchor='w')
        self.refresh()
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.drain)

    # ------------------------------------------------------------------ settings

    def load(self):
        try:
            with open(SETTINGS, encoding='utf-8') as file:
                return json.load(file)
        except (OSError, ValueError):
            return {}

    def save(self):
        self.settings['component'] = self.component.get()
        self.settings['kicad_python'] = self.kicad.get()
        try:
            os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
            with open(SETTINGS, 'w', encoding='utf-8') as file:
                json.dump(self.settings, file, indent=2)
        except OSError:
            pass

    def option(self, stage, flag, default):
        return self.settings.get('stages', {}).get(stage, {}).get(flag, default)

    def configure(self, label):
        options = next(stage[4] for stage in STAGES if stage[0] == label)
        dialog = tk.Toplevel(self.root)
        dialog.title('%s settings' % label)
        dialog.transient(self.root)
        frame = ttk.Frame(dialog, padding=10)
        frame.pack(fill='both')
        values = {}
        for row, (flag, text, kind, default) in enumerate(options):
            value = self.option(label, flag, default)
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
            stage = self.settings.setdefault('stages', {}).setdefault(label, {})
            for flag, text, kind, _ in options:
                value = values[flag].get()
                if kind is int and value.strip():
                    try:
                        value = int(value)
                    except ValueError:
                        messagebox.showerror('Settings', '%s must be a whole number' % text, parent=dialog)
                        return
                stage[flag] = value
            self.save()
            dialog.destroy()

        buttons = ttk.Frame(frame)
        buttons.grid(row=len(options), column=0, columnspan=3, pady=(8, 0), sticky='e')
        ttk.Button(buttons, text='Defaults', command=lambda: [
            values[f].set(d if k is bool else str(d)) for f, _, k, d in options]).pack(side='left', padx=4)
        ttk.Button(buttons, text='OK', command=ok).pack(side='left', padx=4)
        ttk.Button(buttons, text='Cancel', command=dialog.destroy).pack(side='left')
        dialog.grab_set()

    def browse(self, variable):
        types = [('KiCad board', '*.kicad_pcb'), ('All files', '*.*')] if variable.get().endswith('.kicad_pcb') else []
        path = filedialog.askopenfilename(initialdir=os.path.dirname(variable.get()) or ROOT, filetypes=types)
        if path:
            variable.set(os.path.normpath(path))  # Tk returns forward slashes

    # ------------------------------------------------------------------ commands

    def refresh(self):
        names = sorted(name[:-5] for name in os.listdir(COMPONENTS) if name.endswith('.yaml')) \
            if os.path.isdir(COMPONENTS) else []
        self.choice['values'] = names

    def project(self):
        return os.path.join(ROOT, 'Build', 'KiCad', self.component.get())

    def commands(self, label):
        """The command lines a stage runs."""
        _, script, python, argument, options = next(stage for stage in STAGES if stage[0] == label)
        interpreter = sys.executable if python == 'python' else self.kicad.get()
        if script is None:
            return [[sys.executable, os.path.join(ROOT, 'Tools', name)] for name in GENERATORS]
        line = [interpreter, os.path.join(HERE, script)]
        if argument == 'component':
            line.append(self.component.get())
        elif argument == 'project':
            line.append(self.project())
        for flag, _, kind, default in options:
            value = self.option(label, flag, default)
            if kind is bool:
                if value:
                    line.append(flag)
            elif str(value).strip():
                line += [flag, str(value).strip()]
        return [line]

    def run(self, labels):
        if self.process:
            return
        if not self.component.get() and any(label not in ('Generate design', 'Eurocard template') for label in labels):
            messagebox.showerror('EDA', 'Pick a component first')
            return
        self.save()
        self.queue = [line for label in labels for line in self.commands(label)]
        self.log.delete('1.0', 'end')
        self.next()

    def next(self):
        if not self.queue:
            self.finish('done')
            return
        line = self.queue.pop(0)
        self.write('> %s\n' % subprocess.list2cmdline(line))
        self.status.set('running %s' % os.path.basename(line[1]))
        self.stop_button['state'] = 'normal'
        try:
            self.process = subprocess.Popen(line, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            text=True, bufsize=1, encoding='utf-8', errors='replace',
                                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except OSError as error:
            self.write('%s\n' % error)
            self.finish('failed')
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
                    self.queue = []
                    self.finish('failed')
                else:
                    self.next()
            else:
                self.write(item)
        self.root.after(100, self.drain)

    def write(self, text):
        self.log.insert('end', text)
        self.log.see('end')

    def finish(self, status):
        self.status.set(status)
        self.stop_button['state'] = 'disabled'

    def stop(self):
        self.queue = []
        if self.process:
            self.process.kill()
            self.write('stopped\n')

    def open_folder(self):
        folder = self.project()
        if not os.path.isdir(folder):
            messagebox.showinfo('EDA', '%s has no project yet: run Schematic first' % self.component.get())
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
