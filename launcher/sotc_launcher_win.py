#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Shadow of the Colossus launcher for Windows (Tkinter), modelled on bbport_launcher_win.py.

One window for the port's settings: the game dump, the user's own sound modules, saves, modes
(16x AF, 60 FPS), display and diagnostics. They go to sotc.py as environment variables.
Launcher options live in %APPDATA%/sotc-launcher/settings.json.

Frozen with PyInstaller (packaging/windows/package_sotc.sh) the same SotC.exe also runs the
game without the window (`--play`), sotc.py (`--run`) and the preparation scripts (`--script`),
so a packaged port needs no Python installation.
"""
import ctypes
import json
import os
from pathlib import Path
import queue
import runpy
import shutil
import subprocess
import sys
import threading

FROZEN = getattr(sys, 'frozen', False)
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_DIR / 'scripts'))
CONFIG_DIR = Path(os.environ.get('APPDATA', Path.home())) / 'sotc-launcher'
CONFIG_FILE = CONFIG_DIR / 'settings.json'
GAME_ID, GAME_VERSION = 'CUSA08809', '01.01'
SYS_MODULES = ('libSceUlt.sprx', 'libSceNgs2.sprx')
VERSION = '1.1'
MAX_LOG_LINES = 6000
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


# ---------------------------------------------------------------------------------------------
# Command line roles of the frozen executable.

def attach_stdio():
    """A windowed executable starts without sys.stdout; inherited pipes or a console still exist."""
    import msvcrt
    for name, std in (('stdout', -11), ('stderr', -12)):
        if getattr(sys, name) is not None:
            continue
        stream = None
        handle = ctypes.windll.kernel32.GetStdHandle(std)
        if handle and handle != ctypes.c_void_p(-1).value:
            try:
                stream = open(msvcrt.open_osfhandle(handle, os.O_WRONLY), 'w', encoding='utf-8',
                              errors='replace', buffering=1)
            except OSError:
                stream = None
        setattr(sys, name, stream or open(os.devnull, 'w'))


def run_role(argv):
    """--run [args]: sotc.py; --script <file> [args]: a preparation script. Returns an exit code."""
    attach_stdio()
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(line_buffering=True)
        except (AttributeError, ValueError):
            pass
    if argv[0] == '--script':
        path, sys.argv = argv[1], argv[1:]
    else:
        path = str(PORT_DIR / 'sotc.py')
        sys.argv = [path, *argv[1:]]
    try:
        runpy.run_path(path, run_name='__main__')
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
    return 0


def run_command():
    """The command that starts sotc.py: this executable when frozen, else Python."""
    return [sys.executable, '--run'] if FROZEN else [sys.executable, str(PORT_DIR / 'sotc.py')]


# ---------------------------------------------------------------------------------------------
# Texts: Portuguese first, English for every other Windows language.

LANG = 'pt'


def _(pt, en):
    return pt if LANG == 'pt' else en


def windows_language():
    try:
        primary = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff
    except (AttributeError, OSError):
        primary = 0
    return 'pt' if primary == 0x16 else 'en'


# ---------------------------------------------------------------------------------------------
# Settings, passed to sotc.py as environment variables.

APP_DEFAULTS = {'ui_language': '', 'game_dir': str(PORT_DIR.parent / GAME_ID), 'sys_modules': '',
                'user_dir': '', 'af16': True, 'fps60': False, 'fullscreen': False,
                'present_mode': 'Mailbox', 'language': '', 'player_name': '', 'show_fps': False,
                'readbacks': '', 'extra_env': '', 'close_on_play': False, 'gamepad': '',
                'output_res': '1920x1080', 'upscaler': 'fsr1', 'sharpness': '250', 'mouse': True,
                'mouse_sens': '1.0', 'mouse_invert': False, 'neo': False, 'fx_vignette': True,
                'fx_fog': True, 'fx_ssao': True, 'engine_cvars': '', 'perf_culling': True}
# Engine effects verified to change the image when set on the command line (A/B, 08/10/2026).
EFFECTS = [('fx_vignette', ('Vinheta (cantos escurecidos)', 'Vignette (darkened corners)'),
            'r_vignetteOverrideEnable=1 r_vignetteEnable=0'),
           ('fx_fog', ('Névoa atmosférica', 'Atmospheric fog'), 'fog_enable=0'),
           ('fx_ssao', ('Oclusão ambiente (SSAO)', 'Ambient occlusion (SSAO)'), 'AO_Mode=0')]
OUTPUTS = [('1280x720', ('1280 × 720 (720p)', '1280 × 720 (720p)')), ('1920x1080', ('1920 × 1080 (1080p)', '1920 × 1080 (1080p)')),
           ('2560x1440', ('2560 × 1440 (1440p)', '2560 × 1440 (1440p)')), ('3840x2160', ('3840 × 2160 (4K)', '3840 × 2160 (4K)'))]
UPSCALERS = [('fsr1', ('FSR 1 (AMD, qualquer placa)', 'FSR 1 (AMD, any GPU)')),
             ('bilinear', ('Bilinear (simples)', 'Bilinear (plain)'))]
SHARPNESS = [('0', ('Alta', 'High')), ('250', ('Média (padrão)', 'Medium (default)')), ('600', ('Baixa', 'Low')),
             ('off', ('Desligada', 'Off'))]
MOUSE_SENS = [('0.5', ('0,5 (lenta)', '0.5 (slow)')), ('0.75', ('0,75', '0.75')), ('1.0', ('1,0 (padrão)', '1.0 (default)')),
              ('1.5', ('1,5', '1.5')), ('2.0', ('2,0 (rápida)', '2.0 (fast)'))]
PRESENT_MODES = [('Mailbox', ('Mailbox (baixa latência, sem cortes)', 'Mailbox (low latency, no tearing)')),
                 ('Fifo', ('FIFO (VSync)', 'FIFO (VSync)')),
                 ('Immediate', ('Immediate (pode cortar a imagem)', 'Immediate (tearing)'))]
# System language (BB_LANGUAGE, PS4 ids); '' keeps the game's own choice.
LANGUAGES = [('', ('Automático (o do jogo)', 'Automatic (the game\'s)')),
             ('7', ('Português (Portugal)', 'Portuguese (Portugal)')), ('15', ('Português (Brasil)', 'Portuguese (Brazil)')),
             ('1', ('Inglês', 'English')), ('3', ('Espanhol', 'Spanish')), ('2', ('Francês', 'French')),
             ('4', ('Alemão', 'German')), ('5', ('Italiano', 'Italian')), ('0', ('Japonês', 'Japanese'))]
READBACKS = [('', ('Precisas (padrão; o jogo precisa)', 'Precise (default; the game needs them)')),
             ('1', ('Relaxadas (pode travar)', 'Relaxed (may hang)'))]
UI_LANGUAGES = [('', ('Automático', 'Automatic')), ('pt', ('Português', 'Portuguese')), ('en', ('Inglês', 'English'))]
CONTROLS = [('W A S D', ('analógico esquerdo', 'left stick')), (('setas', 'arrows'), ('analógico direito (câmera)', 'right stick (camera)')),
            ('Espaço / Space', '✕'), ('Shift', '○'), ('E', '□'), ('Q', '△'), ('1 / 3', 'L1 / R1'), ('R / F', 'L2 / R2'),
            ('Z / C', 'L3 / R3'), ('Enter', 'Options'), ('I J K L', ('direcional', 'd-pad')),
            ('Tab / Backspace', ('touchpad (esquerda / direita)', 'touchpad (left / right)'))]


def load_json(path, default):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return default


_EBOOT_VERSIONS = {}


def eboot_version(eboot):
    """'01.01' when the eboot holds the EU 1.01 code the modes patch (dumps often keep the base
    game's param.sfo next to the updated eboot), else None. Cached per file state."""
    try:
        stat = eboot.stat()
    except OSError:
        return None
    key = (str(eboot), stat.st_size, stat.st_mtime_ns)
    if key not in _EBOOT_VERSIONS:
        try:
            from prepare import parse_self
            from sotc_modes import is_supported
            elf, _h, ph, _s, _m = parse_self(eboot.read_bytes())
            _EBOOT_VERSIONS[key] = GAME_VERSION if is_supported(elf, ph) else None
        except (OSError, ValueError, ImportError):
            _EBOOT_VERSIONS[key] = None
    return _EBOOT_VERSIONS[key]


def game_info(folder):
    """(title id, game version) of a game folder, or None without eboot.bin."""
    folder = Path(folder or '.')
    if not (folder / 'eboot.bin').is_file():
        return None
    try:
        from prepare import sfo
        values = sfo((folder / 'sce_sys/param.sfo').read_bytes())
        title, version = values.get('TITLE_ID', '?'), values.get('APP_VER', '?')
    except (OSError, ValueError, ImportError):
        title, version = '?', '?'
    return title, eboot_version(folder / 'eboot.bin') or version


def user_dir(s):
    return Path(s.get('user_dir') or PORT_DIR / 'user')


def game_environment(s):
    env = dict(os.environ)
    env['SOTC_GAME_DIR'] = s['game_dir']
    env['SOTC_USER_DIR'] = str(user_dir(s))
    if s['sys_modules']:
        env['SOTC_SYS_MODULES'] = s['sys_modules']
    env['SOTC_MODES'] = ','.join(m for m in ('af16', 'fps60') if s.get(m))
    env['BB_FULLSCREEN'] = '1' if s['fullscreen'] else '0'
    env['BB_PRESENT_MODE'] = s['present_mode']
    if s['language']:
        env['BB_LANGUAGE'] = s['language']
    if str(s['player_name']).strip():
        env['BB_USER_NAME'] = str(s['player_name']).strip()
    if s['show_fps']:
        env['SOTC_FPS'] = '1'
    if s['readbacks']:
        env['BB_READBACKS'] = s['readbacks']
    cvars = [off for key, _t, off in EFFECTS if not s.get(key, True)]
    if s.get('perf_culling', True):
        # From Pink-shadPS4: no GPU index-buffer culling (always both), same image, far fewer
        # GPU results the CPU has to wait for.
        cvars += ['doIndexBufferCulling=0', 'doSunShadowIndexBufferCulling=0']
    cvars += str(s.get('engine_cvars', '')).split()
    env['SOTC_CVARS'] = ' '.join(cvars)
    env['SOTC_NEO'] = '1' if s.get('neo') else '0'
    env['SOTC_OUTPUT_RES'] = s.get('output_res') or '1920x1080'
    env['BB_FSR1'] = '1' if s.get('upscaler', 'fsr1') == 'fsr1' else '0'
    if s.get('sharpness') == 'off':
        env['BB_RCAS'] = '0'
    else:
        env['BB_RCAS'] = '1'
        env['BB_RCAS_ATTENUATION'] = str(s.get('sharpness') or '250')
    env['SOTC_MOUSE'] = '1' if s.get('mouse', True) else '0'
    env['SOTC_MOUSE_SENS'] = str(s.get('mouse_sens') or '1.0')
    env['SOTC_MOUSE_INVERT'] = '1' if s.get('mouse_invert') else '0'
    if str(s.get('gamepad', '')).strip():
        env['BB_GAMEPAD'] = str(s['gamepad']).strip()
    env['BB_CONFIG'] = str(PORT_DIR / 'sotc.ini')
    for item in str(s['extra_env']).split():
        if '=' in item:
            key, value = item.split('=', 1)
            env[key] = value
    env['PYTHONUNBUFFERED'] = '1'
    env['PYTHONIOENCODING'] = 'utf-8'
    return env


def shadps4_saves():
    """Save folders of shadPS4 installations next to the port or in the usual places."""
    roots = [PORT_DIR.parent, Path.home(), Path(os.environ.get('APPDATA', Path.home()))]
    found = []
    for root in roots:
        for pattern in ('*/user/home/*/savedata/' + GAME_ID, '*/*/user/home/*/savedata/' + GAME_ID,
                        'shadPS4/savedata/*/' + GAME_ID):
            try:
                found += [p for p in root.glob(pattern) if p.is_dir()]
            except OSError:
                pass
    return found


# ---------------------------------------------------------------------------------------------
# The window.

BG, PANEL, CARD, LINE = '#0d0f10', '#14171a', '#1b1f23', '#2a3036'
TEXT, MUTED, GOLD, ACCENT, ACCENT_HI = '#e6e2da', '#8f9499', '#d7c08a', '#3e5f73', '#4f7790'


class Launcher:
    def __init__(self, root, tk, ttk, filedialog, messagebox):
        self.tk, self.ttk, self.filedialog, self.messagebox = tk, ttk, filedialog, messagebox
        self.root = root
        self.app = {**APP_DEFAULTS, **load_json(CONFIG_FILE, {})}
        self.vars = {}
        self.process = self.job = None
        self.output = queue.Queue()
        self.ui_calls = queue.Queue()
        self.gpu_text = _('• Verificando a placa de vídeo…', '• Checking the graphics card…')
        self.banner_source = self.banner_image = None
        root.title('Shadow of the Colossus — port nativo' if LANG == 'pt' else 'Shadow of the Colossus — native port')
        root.configure(bg=BG)
        self.dpi = root.winfo_fpixels('1i') / 96.0
        root.geometry(f'{self.px(1080)}x{self.px(700)}')
        root.minsize(self.px(940), self.px(620))
        self.set_icon()
        self.style()
        self.build()
        self.show('play')
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.drain_output)
        threading.Thread(target=self.detect_gpu, daemon=True).start()

    def px(self, size):
        return int(size * self.dpi)

    def set_icon(self):
        try:
            self.root.iconbitmap(default=str(PORT_DIR / 'launcher' / 'sotc.ico'))
            return
        except self.tk.TclError:
            pass
        try:
            self.icon = self.tk.PhotoImage(file=str(Path(self.app['game_dir']) / 'sce_sys' / 'icon0.png'))
            if self.icon.width() > 128:
                self.icon = self.icon.subsample(self.icon.width() // 64)
            self.root.iconphoto(True, self.icon)
        except (self.tk.TclError, OSError):
            pass

    # ---- look --------------------------------------------------------------------------------
    def check_images(self):
        """16 px (DPI-scaled) check box images with a tick: the clam theme draws a cross."""
        n = max(14, int(16 * self.dpi))
        images = []
        for checked in (False, True):
            image = self.tk.PhotoImage(width=n + self.px(8), height=n)  # unset pixels stay transparent
            image.put(GOLD if checked else '#4a535b', to=(0, 0, n, n))
            image.put(ACCENT if checked else CARD, to=(1, 1, n - 1, n - 1))
            if checked:
                x0, y0, x1, y1, x2, y2 = .22 * n, .52 * n, .42 * n, .72 * n, .78 * n, .28 * n
                width = max(1, round(n / 9))
                for t in range(40):
                    f = t / 39
                    for x, y in ((x0 + (x1 - x0) * f, y0 + (y1 - y0) * f), (x1 + (x2 - x1) * f, y1 + (y2 - y1) * f)):
                        image.put('#f4f1ea', to=(int(x), int(y), int(x) + width, int(y) + width))
            images.append(image)
        self.check_off, self.check_on = images

    def style(self):
        ttk = self.ttk
        s = ttk.Style(self.root)
        s.theme_use('clam')
        self.check_images()
        s.element_create('Sotc.indicator', 'image', self.check_off, ('selected', self.check_on), sticky='')
        s.layout('TCheckbutton', [('Checkbutton.padding', {'sticky': 'nswe', 'children': [
            ('Sotc.indicator', {'side': 'left', 'sticky': ''}),
            ('Checkbutton.focus', {'side': 'left', 'sticky': 'w', 'children': [
                ('Checkbutton.label', {'sticky': 'nswe'})]})]})])
        base = ('Segoe UI', 10)
        self.root.option_add('*TCombobox*Listbox.background', CARD)
        self.root.option_add('*TCombobox*Listbox.foreground', TEXT)
        self.root.option_add('*TCombobox*Listbox.selectBackground', ACCENT)
        self.root.option_add('*TCombobox*Listbox.font', base)
        s.configure('.', background=PANEL, foreground=TEXT, fieldbackground=CARD, bordercolor=LINE,
                    lightcolor=LINE, darkcolor=LINE, troughcolor=CARD, focuscolor=GOLD, font=base)
        s.configure('TFrame', background=PANEL)
        s.configure('TLabel', background=PANEL, foreground=TEXT)
        s.configure('Muted.TLabel', background=PANEL, foreground=MUTED, font=('Segoe UI', 9))
        s.configure('Section.TLabel', background=PANEL, foreground=GOLD, font=('Georgia', 13))
        s.configure('TCheckbutton', background=PANEL, foreground=TEXT, padding=(0, 3), indicatorbackground=CARD,
                    indicatorforeground=GOLD)
        s.map('TCheckbutton', background=[('active', PANEL)], indicatorbackground=[('selected', ACCENT)])
        s.configure('Warning.TLabel', background='#2a2312', foreground='#e3c25a', padding=(10, 6))
        s.configure('TCombobox', arrowcolor=GOLD, foreground=TEXT, padding=4)
        s.map('TCombobox', fieldbackground=[('readonly', CARD)], foreground=[('readonly', TEXT)],
              selectbackground=[('readonly', CARD)], selectforeground=[('readonly', TEXT)])
        s.configure('TEntry', foreground=TEXT, insertcolor=TEXT, padding=4)
        s.configure('TButton', background=CARD, foreground=TEXT, padding=(12, 6), borderwidth=1)
        s.map('TButton', background=[('active', LINE), ('disabled', PANEL)], foreground=[('disabled', MUTED)])
        s.configure('Play.TButton', background=ACCENT, foreground='#f4f1ea', font=('Georgia', 15, 'bold'),
                    padding=(36, 10), borderwidth=0)
        s.map('Play.TButton', background=[('active', ACCENT_HI), ('disabled', '#263038')],
              foreground=[('disabled', '#7d858c')])
        s.configure('Vertical.TScrollbar', background=CARD, arrowcolor=GOLD, troughcolor=PANEL, bordercolor=PANEL)

    # ---- widget helpers ----------------------------------------------------------------------
    def var(self, key):
        tk = self.tk
        if key not in self.vars:
            value = self.app.get(key, APP_DEFAULTS[key])
            self.vars[key] = tk.BooleanVar(value=bool(value)) if isinstance(APP_DEFAULTS[key], bool) \
                else tk.StringVar(value=str(value))
        return self.vars[key]

    def choice(self, parent, key, options, width=38):
        var = self.var(key)
        values = [v for v, _t in options]
        box = self.ttk.Combobox(parent, values=[_(*text) for _v, text in options], state='readonly', width=width)
        if var.get() not in values:
            var.set(values[0])

        def show(*_args):
            current = var.get()
            box.current(values.index(current) if current in values else 0)
        show()
        box.bind('<<ComboboxSelected>>', lambda _e: var.set(values[box.current()]))
        var.trace_add('write', show)
        return box

    def next_row(self, parent):
        return parent.grid_size()[1]

    def row(self, parent, title, widget, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        ttk.Label(parent, text=title).grid(row=r, column=0, sticky='nw', padx=(0, 18), pady=(8, 0))
        widget.grid(row=r, column=1, sticky='w', pady=(5, 0))
        if hint:
            ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(560), justify='left').grid(
                row=r + 1, column=1, sticky='w', pady=(2, 2))
        return widget

    def check(self, parent, key, title, hint=None):
        ttk = self.ttk
        r = self.next_row(parent)
        ttk.Checkbutton(parent, text=title, variable=self.var(key)).grid(row=r, column=0, columnspan=2, sticky='w', pady=(4, 0))
        if hint:
            ttk.Label(parent, text=hint, style='Muted.TLabel', wraplength=self.px(640), justify='left').grid(
                row=r + 1, column=0, columnspan=2, sticky='w', padx=(26, 0))

    def note(self, parent, text, top=12):
        self.ttk.Label(parent, text=text, style='Muted.TLabel', wraplength=self.px(680), justify='left').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 0))

    def section(self, parent, title, top=18):
        self.ttk.Label(parent, text=title, style='Section.TLabel').grid(
            row=self.next_row(parent), column=0, columnspan=2, sticky='w', pady=(top, 2))

    def folder(self, parent, key, title, prompt, hint=None, on_change=None):
        ttk = self.ttk
        var = self.var(key)
        holder = ttk.Frame(parent)
        ttk.Entry(holder, textvariable=var, width=54).pack(side='left')

        def browse():
            chosen = self.filedialog.askdirectory(title=prompt, initialdir=var.get() or str(PORT_DIR))
            if chosen:
                var.set(str(Path(chosen)))
        ttk.Button(holder, text=_('Procurar…', 'Browse…'), command=browse).pack(side='left', padx=(6, 0))
        ttk.Button(holder, text=_('Abrir', 'Open'), command=lambda: self.open_path(var.get() or (
            user_dir(self.app) if key == 'user_dir' else PORT_DIR))).pack(side='left', padx=(6, 0))
        if on_change:
            var.trace_add('write', lambda *_a: on_change())
        return self.row(parent, title, holder, hint)

    def scrolled_page(self, name, title, subtitle):
        tk, ttk = self.tk, self.ttk
        outer = ttk.Frame(self.content)
        head = ttk.Frame(outer, padding=(28, 22, 28, 6))
        head.pack(fill='x')
        ttk.Label(head, text=title, background=PANEL, foreground=TEXT, font=('Georgia', 20)).pack(anchor='w')
        ttk.Label(head, text=subtitle, style='Muted.TLabel').pack(anchor='w')
        canvas = tk.Canvas(outer, bg=PANEL, highlightthickness=0, bd=0)
        bar = ttk.Scrollbar(outer, orient='vertical', command=canvas.yview)
        inner = ttk.Frame(canvas, padding=(28, 0, 28, 24))
        inner.columnconfigure(1, weight=1)
        window = canvas.create_window(0, 0, window=inner, anchor='nw')
        inner.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(window, width=e.width))
        canvas.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        canvas.pack(side='left', fill='both', expand=True)
        outer.scroll = lambda units: canvas.yview_scroll(units, 'units') if inner.winfo_height() > canvas.winfo_height() else None
        self.pages[name] = outer
        return inner

    # ---- layout ------------------------------------------------------------------------------
    def build(self):
        tk, ttk = self.tk, self.ttk
        side = tk.Frame(self.root, bg=BG, width=self.px(220))
        side.pack(side='left', fill='y')
        side.pack_propagate(False)
        tk.Label(side, text='SHADOW OF\nTHE COLOSSUS', bg=BG, fg=GOLD, font=('Georgia', 15), justify='left').pack(
            anchor='w', padx=22, pady=(24, 0))
        tk.Label(side, text=_('port nativo · Windows', 'native port · Windows') + f'  ·  v{VERSION}', bg=BG, fg=MUTED,
                 font=('Segoe UI', 9)).pack(anchor='w', padx=22, pady=(2, 20))
        self.nav, self.current_page = {}, None
        for name, title in (('play', _('Jogar', 'Play')), ('game', _('Jogo e saves', 'Game & saves')),
                            ('graphics', _('Imagem e FPS', 'Display & FPS')), ('controls', _('Controles', 'Controls')),
                            ('advanced', _('Avançado', 'Advanced')), ('log', _('Registro', 'Log'))):
            item = tk.Label(side, text='    ' + title, bg=BG, fg=TEXT, anchor='w', font=('Segoe UI', 11),
                            pady=10, cursor='hand2')
            item.pack(fill='x')
            item.bind('<Button-1>', lambda _e, n=name: self.show(n))
            item.bind('<Enter>', lambda _e, n=name: n != self.current_page and self.nav[n].configure(bg='#171b1f'))
            item.bind('<Leave>', lambda _e, n=name: n != self.current_page and self.nav[n].configure(bg=BG))
            self.nav[name] = item
        tk.Frame(side, bg=BG).pack(fill='both', expand=True)
        tk.Label(side, text=_('Sem controle, o teclado\nfunciona (veja Controles).',
                              'Without a gamepad the\nkeyboard works (see Controls).'),
                 bg=BG, fg=MUTED, font=('Segoe UI', 9), justify='left').pack(anchor='w', padx=22, pady=(0, 18))

        right = tk.Frame(self.root, bg=PANEL)
        right.pack(side='left', fill='both', expand=True)
        bar = tk.Frame(right, bg=BG, height=72)
        bar.pack(fill='x', side='bottom')
        bar.pack_propagate(False)
        self.status = tk.Label(bar, text='', bg=BG, fg=MUTED, font=('Segoe UI', 10), anchor='w', justify='left')
        self.status.pack(side='left', padx=24)
        self.play_button = ttk.Button(bar, text=_('JOGAR', 'PLAY'), style='Play.TButton', command=self.play)
        self.play_button.pack(side='right', padx=(10, 24), pady=11)
        self.stop_button = ttk.Button(bar, text=_('Parar', 'Stop'), command=self.stop, state='disabled')
        self.stop_button.pack(side='right', pady=11)
        self.content = tk.Frame(right, bg=PANEL)
        self.content.pack(fill='both', expand=True)

        self.pages = {}
        self.build_play()
        self.build_game()
        self.build_graphics()
        self.build_controls()
        self.build_advanced()
        self.build_log()
        self.root.bind_all('<MouseWheel>', self.wheel)
        self.refresh_status()

    def wheel(self, event):
        page = self.pages.get(self.current_page)
        if hasattr(page, 'scroll') and not isinstance(event.widget, (self.tk.Text, self.ttk.Combobox)) \
                and 'popdown' not in str(event.widget):
            page.scroll(int(-event.delta / 120))

    def show(self, name):
        for page in self.pages.values():
            page.pack_forget()
        self.pages[name].pack(fill='both', expand=True)
        self.current_page = name
        for n, item in self.nav.items():
            item.configure(bg=PANEL if n == name else BG, fg=GOLD if n == name else TEXT)
        if name == 'play':
            self.refresh_status()

    def build_play(self):
        tk, ttk = self.tk, self.ttk
        page = tk.Frame(self.content, bg=PANEL)
        self.pages['play'] = page
        self.banner = tk.Canvas(page, height=self.px(290), bg=BG, highlightthickness=0, bd=0)
        self.banner.pack(fill='x')
        self.banner.bind('<Configure>', lambda _e: self.draw_banner())
        body = ttk.Frame(page, padding=(28, 12, 28, 8))
        body.pack(fill='both', expand=True)
        body.columnconfigure(0, weight=3)
        body.columnconfigure(1, weight=2)
        info = ttk.Frame(body)
        info.grid(row=0, column=0, sticky='nw', padx=(0, 24))
        ttk.Label(info, text=_('Verificação', 'Ready check'), style='Section.TLabel').pack(anchor='w', pady=(0, 6))
        self.checks = {}
        for key in ('game', 'sound', 'saves', 'gpu'):
            self.checks[key] = ttk.Label(info, text='', justify='left', wraplength=self.px(440))
            self.checks[key].pack(anchor='w', pady=3)
        quick = ttk.Frame(body)
        quick.grid(row=0, column=1, sticky='nw')
        ttk.Label(quick, text=_('Ajustes rápidos', 'Quick settings'), style='Section.TLabel').grid(
            row=0, column=0, columnspan=2, sticky='w', pady=(0, 2))
        self.row(quick, _('Saída', 'Output'), self.choice(quick, 'output_res', OUTPUTS, 24))
        self.row(quick, _('Upscaler', 'Upscaler'), self.choice(quick, 'upscaler', UPSCALERS, 24))
        self.check(quick, 'fps60', _('60 FPS (experimental)', '60 FPS (experimental)'))
        self.check(quick, 'af16', _('Texturas nítidas (filtro 16x)', 'Sharp textures (16x filtering)'))
        self.check(quick, 'fullscreen', _('Tela cheia', 'Fullscreen'))
        for key in ('af16', 'fps60', 'fullscreen', 'output_res', 'upscaler'):
            self.vars[key].trace_add('write', lambda *_a: self.refresh_status())

    def draw_banner(self):
        """The dump's own art (sce_sys/pic1.png) under the title."""
        tk, c = self.tk, self.banner
        c.delete('all')
        w, h = max(c.winfo_width(), 400), int(c['height'])
        art = Path(self.var('game_dir').get() or '.') / 'sce_sys' / 'pic1.png'
        if self.banner_source != art:
            self.banner_source, self.banner_image, self.banner_size = art, None, None
            try:
                self.banner_image = tk.PhotoImage(file=str(art))
            except (tk.TclError, OSError):
                pass
        if self.banner_image:
            if self.banner_size != w:
                self.banner_size, self.banner_scaled = w, self.scaled_art(art, w)
            c.create_image(w // 2, int(h * 0.5), image=self.banner_scaled)
            for i, stipple in enumerate(('gray12', 'gray25', 'gray50', 'gray75')):
                c.create_rectangle(0, h - 120 + i * 24, w, h - 96 + i * 24, fill=PANEL, outline='', stipple=stipple)
            c.create_rectangle(0, h - 24, w, h, fill=PANEL, outline='')
        else:
            c.create_text(30, h - 80, text='Shadow of the Colossus', anchor='w', fill='#f2ead9', font=('Georgia', 32))
            c.create_text(w // 2, h // 2 - 40, text=_('Escolha a pasta do jogo (Jogo e saves)',
                                                       'Choose your game folder (Game & saves)'),
                          fill=MUTED, font=('Segoe UI', 11))
        info = game_info(self.var('game_dir').get())
        sub = (_('{} · versão do jogo {}', '{} · game version {}').format(*info) if info
               else _('Pasta do jogo não escolhida', 'Game folder not set'))
        c.create_text(33, h - 30, text=sub, anchor='w', fill=GOLD, font=('Segoe UI', 11))

    def scaled_art(self, path, width):
        try:
            from PIL import Image, ImageTk
            with Image.open(path) as image:
                height = round(image.height * width / image.width)
                return ImageTk.PhotoImage(image.convert('RGB').resize((width, height), Image.LANCZOS))
        except (ImportError, OSError):
            factor = max(1, round(self.banner_image.width() / max(width, 1)))
            return self.banner_image.subsample(factor) if factor > 1 else self.banner_image

    def build_game(self):
        ttk = self.ttk
        f = self.scrolled_page('game', _('Jogo e saves', 'Game & saves'),
                               _('O seu dump do jogo, os módulos de som do seu console e a pasta dos saves.',
                                 'Your game dump, your console\'s sound modules and the saves folder.'))
        self.section(f, _('Jogo', 'Game'), top=4)
        self.folder(f, 'game_dir', _('Pasta do jogo', 'Game folder'), _('Pasta com eboot.bin', 'Folder with eboot.bin'),
                    _('O dump CUSA08809 (versão europeia 1.01), com eboot.bin, sce_module e sce_sys.',
                      'The CUSA08809 dump (EU version 1.01) with eboot.bin, sce_module and sce_sys.'),
                    on_change=self.game_changed)
        self.section(f, _('Som', 'Sound'))
        self.folder(f, 'sys_modules', _('Módulos de som', 'Sound modules'),
                    _('Pasta com libSceNgs2.sprx e libSceUlt.sprx', 'Folder with libSceNgs2.sprx and libSceUlt.sprx'),
                    _('libSceNgs2.sprx e libSceUlt.sprx do SEU console, decriptados (a pasta user/sys_modules do '
                      'shadPS4). São firmware da Sony e nunca vêm com o port. Sem eles o jogo roda sem som.',
                      'libSceNgs2.sprx and libSceUlt.sprx from YOUR console, decrypted (shadPS4\'s user/sys_modules). '
                      'They are Sony firmware and never come with the port. Without them the game is silent.'),
                    on_change=self.refresh_status)
        self.section(f, _('Saves', 'Saves'))
        self.folder(f, 'user_dir', _('Pasta de dados', 'Data folder'), _('Pasta dos saves', 'Saves folder'),
                    _('Saves, cache de shaders e capturas. Vazio = a pasta user ao lado do port.',
                      'Saves, shader cache and screenshots. Empty = the user folder next to the port.'),
                    on_change=self.refresh_status)
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        ttk.Button(holder, text=_('Importar save do shadPS4…', 'Import a shadPS4 save…'), command=self.import_save).pack(side='left')
        ttk.Label(holder, text=_('Copia a pasta savedata/CUSA08809 do shadPS4 para cá.',
                                 'Copies shadPS4\'s savedata/CUSA08809 folder here.'), style='Muted.TLabel').pack(side='left', padx=10)
        self.section(f, _('Sistema', 'System'))
        self.row(f, _('Idioma do sistema', 'System language'), self.choice(f, 'language', LANGUAGES),
                 _('O idioma que o jogo vê no "console".', 'The language the game sees on the "console".'))
        self.row(f, _('Nome do jogador', 'Player name'), ttk.Entry(f, textvariable=self.var('player_name'), width=30))

    def build_graphics(self):
        f = self.scrolled_page('graphics', _('Imagem e FPS', 'Display & FPS'),
                               _('Os modos vêm do fork shadPS4 SotC e valem para a versão EU 1.01.',
                                 'The modes come from the shadPS4 SotC fork and apply to EU version 1.01.'))
        self.section(f, _('Modos', 'Modes'), top=4)
        self.check(f, 'af16', _('Texturas nítidas (filtro anisotrópico 16x)', 'Sharp textures (16x anisotropic filtering)'),
                   _('Chão, grama e rochas ficam nítidos à distância, quase sem custo.',
                     'Ground, grass and rocks stay sharp at a distance, at almost no cost.'))
        self.check(f, 'fps60', _('60 FPS (experimental)', '60 FPS (experimental)'),
                   _('Tira o limite de 30 FPS do jogo; o vblank vai a 120 Hz. Mediu 38 FPS no campo e ~25 no templo.',
                     'Removes the game\'s 30 FPS cap; vblank at 120 Hz. Measured 38 FPS in the open world, ~25 at the temple.'))
        self.section(f, _('Resolução', 'Resolution'))
        self.row(f, _('Saída', 'Output'), self.choice(f, 'output_res', OUTPUTS),
                 _('O jogo renderiza em 1080p, como no PS4; a imagem é ampliada ou reduzida para a saída. '
                   'Num monitor menor que a saída, a janela cabe na tela e a tela cheia usa o monitor todo.',
                   'The game renders at 1080p, as on PS4; the image is scaled to the output. On a monitor '
                   'smaller than the output the window fits the screen and fullscreen uses the whole monitor.'))
        self.row(f, _('Upscaler', 'Upscaler'), self.choice(f, 'upscaler', UPSCALERS),
                 _('FSR 1 amplia com bordas nítidas (acima de 1080p).', 'FSR 1 upscales with sharp edges (above 1080p).'))
        self.row(f, _('Nitidez (RCAS)', 'Sharpening (RCAS)'), self.choice(f, 'sharpness', SHARPNESS))
        self.section(f, _('Modo PS4 Pro', 'PS4 Pro mode'))
        self.check(f, 'neo', _('Apresentar-se como PS4 Pro', 'Present as a PS4 Pro'),
                   _('O jogo passa a oferecer, no menu Opções > Apresentação, o Modo Grafismo do PS4 Pro: '
                     'Cinematográfico (1440p, mais detalhe) ou Desempenho. Mais pesado: no campo aberto mediu ~25 FPS '
                     'contra ~38 no modo normal.',
                     'The game then offers, in Options > Presentation, the PS4 Pro graphics mode: Cinematic '
                     '(1440p, more detail) or Performance. Heavier: ~25 FPS in the open world against ~38 normally.'))
        self.section(f, _('Desempenho', 'Performance'))
        self.check(f, 'perf_culling', _('Culling sem a GPU (recomendado)', 'Culling without the GPU (recommended)'),
                   _('Desliga o culling de índices feito na GPU (técnica do Pink-shadPS4): mesma imagem e bem menos '
                     'esperas pela GPU. No santuário: ~22 → ~28 FPS.',
                     'Turns off the GPU index-buffer culling (Pink-shadPS4): same image, far fewer GPU waits. '
                     'Sanctuary: ~22 → ~28 FPS.'))
        self.section(f, _('Efeitos', 'Effects'))
        for key, title, _off in EFFECTS:
            self.check(f, key, _(*title))
        self.note(f, _('O motion blur, o brilho e os filtros ficam no menu do próprio jogo (Opções > Apresentação).',
                       "Motion blur, brightness and filters are in the game's own menu (Options > Presentation)."), top=4)
        self.section(f, _('Tela', 'Display'))
        self.check(f, 'fullscreen', _('Tela cheia', 'Fullscreen'))
        self.row(f, _('Apresentação', 'Presentation'), self.choice(f, 'present_mode', PRESENT_MODES))

    def build_controls(self):
        ttk = self.ttk
        f = self.scrolled_page('controls', _('Controles', 'Controls'),
                               _('Um controle (DualShock 4, DualSense, Xbox…) é usado automaticamente.',
                                 'A gamepad (DualShock 4, DualSense, Xbox…) is used automatically.'))
        self.section(f, _('Controle', 'Gamepad'), top=4)
        self.row(f, _('Controle preferido', 'Preferred gamepad'), ttk.Entry(f, textvariable=self.var('gamepad'), width=30),
                 _('Nome (ou parte) do controle a usar quando houver vários; vazio = o primeiro.',
                   'Name (or part of it) of the gamepad to use when several are connected; empty = the first.'))
        self.section(f, _('Mouse', 'Mouse'))
        self.check(f, 'mouse', _('Câmera no mouse', 'Mouse camera'),
                   _('Clique na janela do jogo para capturar o mouse; Esc solta. Botão esquerdo = R1 (agarrar), '
                     'direito = □ (atacar), meio = L1. Remapeie com mouse.<entrada>= no sotc.ini.',
                     'Click the game window to capture the mouse; Esc releases it. Left button = R1 (grip), right = □ '
                     '(attack), middle = L1. Remap with mouse.<input>= in sotc.ini.'))
        self.row(f, _('Sensibilidade', 'Sensitivity'), self.choice(f, 'mouse_sens', MOUSE_SENS, 20))
        self.check(f, 'mouse_invert', _('Inverter o eixo vertical', 'Invert the vertical axis'))
        self.section(f, _('Teclado (funciona junto com o controle)', 'Keyboard (works next to a gamepad)'))
        for key, action in CONTROLS:
            key = _(*key) if isinstance(key, tuple) else key
            action = _(*action) if isinstance(action, tuple) else action
            r = self.next_row(f)
            ttk.Label(f, text=key, foreground=GOLD).grid(row=r, column=0, sticky='w', padx=(0, 24), pady=2)
            ttk.Label(f, text=action).grid(row=r, column=1, sticky='w', pady=2)
        self.note(f, _('Para remapear, ponha linhas no sotc.ini da pasta do port, por exemplo key.cross=Space,J '
                       'ou pad.circle=b. Entradas: cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad '
                       'touchpad_right up down left right move_up/down/left/right look_up/down/left/right.',
                       'To remap, add lines to sotc.ini in the port folder, e.g. key.cross=Space,J or pad.circle=b. '
                       'Inputs: cross circle square triangle l1 r1 l2 r2 l3 r3 options touchpad touchpad_right up '
                       'down left right move_up/down/left/right look_up/down/left/right.'))

    def build_advanced(self):
        ttk = self.ttk
        f = self.scrolled_page('advanced', _('Avançado', 'Advanced'),
                               _('Opções do lançador e diagnóstico.', 'Launcher options and diagnostics.'))
        self.section(f, _('Lançador', 'Launcher'), top=4)
        self.row(f, _('Idioma do lançador', 'Launcher language'), self.choice(f, 'ui_language', UI_LANGUAGES),
                 _('Vale na próxima vez que o lançador abrir.', 'Applies when the launcher opens again.'))
        self.check(f, 'close_on_play', _('Fechar o lançador quando o jogo abrir', 'Close the launcher when the game starts'))
        holder = ttk.Frame(f)
        holder.grid(row=self.next_row(f), column=0, columnspan=2, sticky='w', pady=(10, 0))
        ttk.Button(holder, text=_('Atalho na área de trabalho', 'Desktop shortcut'), command=self.shortcut).pack(side='left')
        ttk.Button(holder, text=_('Pasta do port', 'Port folder'), command=lambda: self.open_path(PORT_DIR)).pack(side='left', padx=6)
        ttk.Button(holder, text=_('Limpar cache de shaders', 'Clear shader cache'), command=self.clear_cache).pack(side='left')
        self.section(f, _('Diagnóstico', 'Diagnostics'))
        self.check(f, 'show_fps', _('FPS no registro (a cada 5 s)', 'FPS in the log (every 5 s)'))
        self.row(f, _('Leituras da GPU', 'GPU readbacks'), self.choice(f, 'readbacks', READBACKS))
        self.row(f, _('Cvars do motor', 'Engine cvars'), ttk.Entry(f, textvariable=self.var('engine_cvars'), width=58),
                 _('Avançado: NOME=valor separados por espaço, acrescentados à linha de comando do jogo.',
                   "Advanced: NAME=value separated by spaces, added to the game's command line."))
        self.row(f, _('Variáveis extras', 'Extra variables'), ttk.Entry(f, textvariable=self.var('extra_env'), width=58),
                 _('Pares NOME=valor separados por espaço (veja o README).',
                   'NAME=value pairs separated by spaces (see the README).'))

    def build_log(self):
        tk, ttk = self.tk, self.ttk
        page = ttk.Frame(self.content, padding=(20, 14, 20, 8))
        self.pages['log'] = page
        top = ttk.Frame(page)
        top.pack(fill='x', pady=(0, 8))
        ttk.Label(top, text=_('Registro', 'Log'), font=('Georgia', 20)).pack(side='left')
        ttk.Button(top, text=_('Copiar', 'Copy'), command=self.copy_log).pack(side='right')
        ttk.Button(top, text=_('Limpar', 'Clear'), command=lambda: self.set_log('')).pack(side='right', padx=6)
        self.log = tk.Text(page, wrap='none', bg='#090a0b', fg='#c9ccd0', insertbackground=TEXT, relief='flat',
                           font=('Consolas', 9), padx=8, pady=6, state='disabled', highlightthickness=0)
        bar = ttk.Scrollbar(page, command=self.log.yview)
        self.log.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        self.log.pack(fill='both', expand=True)

    # ---- state -------------------------------------------------------------------------------
    def game_changed(self):
        self.banner_source = None
        self.draw_banner()
        self.set_icon()
        self.refresh_status()

    def refresh_status(self):
        info = game_info(self.var('game_dir').get())
        if not info:
            game = _('✗ Não há eboot.bin na pasta do jogo (Jogo e saves)', '✗ No eboot.bin in the game folder (Game & saves)')
        elif info == (GAME_ID, GAME_VERSION):
            game = _('✓ {} versão {}: todos os modos disponíveis', '✓ {} version {}: every mode available').format(*info)
        else:
            game = _('⚠ {} versão {}: o port foi feito para {} {}; os modos não se aplicam',
                     '⚠ {} version {}: the port targets {} {}; the modes do not apply').format(*info, GAME_ID, GAME_VERSION)
        modules = Path(self.var('sys_modules').get() or '.')
        if self.var('sys_modules').get() and all((modules / m).is_file() for m in SYS_MODULES):
            sound = _('✓ Módulos de som encontrados', '✓ Sound modules found')
        else:
            sound = _('⚠ Sem os módulos de som do seu console: o jogo fica mudo (Jogo e saves)',
                      '⚠ No sound modules from your console: the game is silent (Game & saves)')
        user = user_dir({'user_dir': self.var('user_dir').get()})
        saves = user / 'savedata' / '1' / GAME_ID
        save = (_('✓ Saves em {}', '✓ Saves in {}').format(saves) if saves.is_dir() and any(saves.iterdir())
                else _('• Ainda sem saves: o jogo cria em {}', '• No saves yet: the game creates them in {}').format(saves))
        for key, text in (('game', game), ('sound', sound), ('saves', save), ('gpu', self.gpu_text)):
            self.checks[key].configure(text=text, foreground=MUTED if text.startswith('•') else
                                       '#d9b441' if text.startswith('⚠') else '#d36b5c' if text.startswith('✗') else TEXT)
        if not self.process:
            self.play_button.configure(state='normal' if info else 'disabled')
            self.status.configure(text=self.summary() if info else _('Escolha primeiro a pasta do jogo.',
                                                                     'Choose the game folder first.'), fg=MUTED)

    def summary(self):
        parts = [_('60 FPS', '60 FPS') if self.var('fps60').get() else _('30 FPS (como no PS4)', '30 FPS (as on PS4)'),
                 self.var('output_res').get().replace('x', ' × '),
                 'FSR 1' if self.var('upscaler').get() == 'fsr1' else 'bilinear']
        if self.var('af16').get():
            parts.append(_('AF 16x', '16x AF'))
        if self.var('neo').get():
            parts.append('PS4 Pro')
        parts.append(_('tela cheia', 'fullscreen') if self.var('fullscreen').get() else _('janela', 'window'))
        return '   ·   '.join(parts)

    def detect_gpu(self):
        exe = PORT_DIR / 'bin' / 'bb-gpu-capabilities.exe'
        if not exe.is_file():
            exe = PORT_DIR / 'out' / 'bb-gpu-capabilities.exe'
        env = dict(os.environ)
        if not (exe.parent / 'SDL3.dll').is_file():
            clang64 = Path(os.environ.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
        text = _('• Placa de vídeo: não verificada', '• Graphics card: not checked')
        try:
            result = subprocess.run([str(exe), '--live-resolution'], capture_output=True, text=True, timeout=30,
                                    env=env, creationflags=NO_WINDOW)
            names = [line[5:].split(':')[0] for line in result.stderr.splitlines() if line.startswith('GPU: ')]
            if names:
                text = _('✓ Placa de vídeo: {}', '✓ Graphics card: {}').format(names[0])
            elif result.returncode:
                text = _('✗ Nenhuma placa com Vulkan 1.3 (atualize o driver)',
                         '✗ No Vulkan 1.3 graphics card found (update the driver)')
        except (OSError, subprocess.TimeoutExpired):
            pass
        self.gpu_text = text
        self.ui_calls.put(self.refresh_status)

    def collect(self):
        for key, var in self.vars.items():
            self.app[key] = var.get()
        CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        CONFIG_FILE.write_text(json.dumps(self.app, indent=2, ensure_ascii=False), encoding='utf-8')

    def import_save(self):
        found = shadps4_saves()
        initial = str(found[0]) if found else str(PORT_DIR.parent)
        chosen = self.filedialog.askdirectory(title=_('Pasta savedata/CUSA08809 do shadPS4', "shadPS4's savedata/CUSA08809 folder"),
                                              initialdir=initial)
        if not chosen:
            return
        source = Path(chosen)
        if source.name != GAME_ID and (source / GAME_ID).is_dir():
            source = source / GAME_ID
        target = user_dir({'user_dir': self.var('user_dir').get()}) / 'savedata' / '1' / GAME_ID
        if target.exists() and any(target.iterdir()):
            if not self.messagebox.askyesno('Shadow of the Colossus', _(
                    'Já há saves em {}. Substituir? (uma cópia fica em {}.bak)',
                    'There are saves in {} already. Replace them? (a copy stays in {}.bak)').format(target, target.name)):
                return
            backup = target.with_name(target.name + '.bak')
            shutil.rmtree(backup, ignore_errors=True)
            target.rename(backup)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, target)
        self.refresh_status()
        self.messagebox.showinfo('Shadow of the Colossus', _('Save importado para {}.', 'Save imported to {}.').format(target))

    # ---- game process ------------------------------------------------------------------------
    def play(self):
        if self.process:
            return
        self.collect()
        if not game_info(self.app['game_dir']):
            self.messagebox.showerror('Shadow of the Colossus', _('Escolha a pasta do jogo com eboot.bin (CUSA08809).',
                                                                  'Choose the game folder with eboot.bin (CUSA08809).'))
            self.show('game')
            return
        self.set_log('')
        try:
            self.process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(self.app),
                                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        except OSError as error:
            self.append(_('Não foi possível iniciar: {}', 'Could not start: {}').format(error) + '\n')
            self.process = None
            return
        self.job = GameJob(self.process)
        threading.Thread(target=self.read_output, args=(self.process,), daemon=True).start()
        self.play_button.configure(state='disabled')
        self.stop_button.configure(state='normal')
        self.status.configure(text=_('Preparando o jogo; ele abre na própria janela…',
                                     'Preparing the game; it opens in its own window…'), fg=GOLD)
        if self.app.get('close_on_play'):
            self.root.after(5000, self.root.destroy)

    def read_output(self, process):
        for raw in iter(process.stdout.readline, b''):
            self.output.put(raw.decode('utf-8', errors='replace'))
        self.output.put((process.wait(),))

    def drain_output(self):
        while not self.ui_calls.empty():
            self.ui_calls.get_nowait()()
        try:
            for _i in range(500):
                item = self.output.get_nowait()
                if isinstance(item, tuple):
                    self.append(_('\n— o jogo terminou (código {}) —\n', '\n— the game exited (code {}) —\n').format(item[0]))
                    self.process = None
                    if self.job:
                        self.job.close()
                    self.stop_button.configure(state='disabled')
                    self.refresh_status()
                else:
                    if 'Entering original x86-64 code' in item:
                        self.status.configure(text=_('O jogo está rodando.', 'The game is running.'), fg=GOLD)
                    self.append(item)
        except queue.Empty:
            pass
        self.root.after(100, self.drain_output)

    def stop(self):
        if self.job:
            self.job.terminate()

    def append(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text)
        lines = int(self.log.index('end-1c').split('.')[0])
        if lines > MAX_LOG_LINES:
            self.log.delete('1.0', f'{lines - MAX_LOG_LINES}.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def set_log(self, text):
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        self.log.insert('end', text)
        self.log.configure(state='disabled')

    def copy_log(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.log.get('1.0', 'end'))

    def open_path(self, path):
        path = Path(path or PORT_DIR)
        if not path.exists():
            path.mkdir(parents=True, exist_ok=True)
        os.startfile(str(path))

    def clear_cache(self):
        if self.process:
            return
        for name in ('cache', 'shader'):
            shutil.rmtree(user_dir(self.app) / name, ignore_errors=True)
        self.messagebox.showinfo('Shadow of the Colossus', _(
            'Cache de shaders limpo. A próxima partida engasga alguns minutos enquanto ele é refeito.',
            'Shader cache cleared. The next start stutters for a few minutes while it is rebuilt.'))

    def shortcut(self):
        if FROZEN:
            target, arguments, icon = sys.executable, '', f'{sys.executable},0'
        else:
            pythonw = Path(sys.executable).with_name('pythonw.exe')
            target = str(pythonw if pythonw.exists() else sys.executable)
            arguments, icon = Path(__file__).resolve(), PORT_DIR / 'launcher' / 'sotc.ico'
        script = ('$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath("Desktop")'
                  '+"\\Shadow of the Colossus.lnk");'
                  f'$s.TargetPath="{target}";$s.WorkingDirectory="{PORT_DIR}";$s.IconLocation="{icon}";'
                  + (f"$s.Arguments='\"{arguments}\"';" if arguments else '') + '$s.Save()')
        result = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True,
                                creationflags=NO_WINDOW)
        if result.returncode == 0:
            self.messagebox.showinfo('Shadow of the Colossus', _('Atalho criado na área de trabalho.',
                                                                 'Shortcut created on the desktop.'))
        else:
            self.messagebox.showerror('Shadow of the Colossus', result.stderr.decode(errors='replace')[:400])

    def close(self):
        try:
            self.collect()
        except Exception:  # never keep the window open over a settings problem
            pass
        self.root.destroy()


class GameJob:
    """A Windows job holding sotc.py and the bb-probe.exe it starts (Stop ends both)."""

    def __init__(self, process):
        kernel32 = ctypes.windll.kernel32
        kernel32.CreateJobObjectW.restype = ctypes.c_void_p
        self.handle = kernel32.CreateJobObjectW(None, None)
        if self.handle:
            kernel32.AssignProcessToJobObject(ctypes.c_void_p(self.handle), ctypes.c_void_p(int(process._handle)))

    def terminate(self):
        if self.handle:
            ctypes.windll.kernel32.TerminateJobObject(ctypes.c_void_p(self.handle), 1)

    def close(self):
        if self.handle:
            ctypes.windll.kernel32.CloseHandle(ctypes.c_void_p(self.handle))
            self.handle = None


def play_without_window(settings):
    """--play: the game with the saved settings (shortcuts, Steam); output to user/last_run.log."""
    attach_stdio()
    log_dir = user_dir(settings)
    log_dir.mkdir(parents=True, exist_ok=True)
    with open(log_dir / 'last_run.log', 'w', encoding='utf-8', buffering=1) as log:
        process = subprocess.Popen(run_command(), cwd=PORT_DIR, env=game_environment(settings),
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        for raw in iter(process.stdout.readline, b''):
            text = raw.decode('utf-8', errors='replace')
            log.write(text)
            try:
                sys.stdout.write(text)
            except (OSError, ValueError):
                pass
        return process.wait()


def main():
    global LANG
    args = sys.argv[1:]
    if args and args[0] in ('--run', '--script'):
        sys.exit(run_role(args))
    settings = {**APP_DEFAULTS, **load_json(CONFIG_FILE, {})}
    LANG = settings.get('ui_language') or windows_language()
    if '--play' in args and (Path(settings['game_dir'] or '.') / 'eboot.bin').is_file():
        sys.exit(play_without_window(settings))
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    root = tk.Tk()
    Launcher(root, tk, ttk, filedialog, messagebox)
    root.mainloop()


if __name__ == '__main__':
    main()
