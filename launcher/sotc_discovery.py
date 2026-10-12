# SPDX-License-Identifier: GPL-2.0-or-later
"""Finds what the launcher needs on this PC: PS4 game folders (a library like shadPS4's game
folders), the user's own decrypted sound modules (libSceNgs2/libSceUlt from a shadPS4 install)
and shadPS4 saves of the game. Nothing here comes with the port: it only looks for the user's
own files."""
import ctypes
import os
from pathlib import Path
import string
import time

GAME_ID = 'CUSA08809'
SYS_MODULES = ('libSceUlt.sprx', 'libSceNgs2.sprx')

# Folders never worth entering on a drive scan.
SKIP = {'windows', '$recycle.bin', 'system volume information', 'programdata', 'recovery',
        'perflogs', 'msocache', '$windows.~bt', '$windows.~ws', 'windows.old', 'node_modules',
        '.git', '__pycache__', 'winsxs', 'microsoft', 'nvidia corporation', 'intel', 'amd',
        'appdata', 'steamapps', 'system32', 'syswow64', 'driverstore', 'installer'}


def game_record(folder):
    """{'path', 'id', 'title', 'version', 'icon'} of a PS4 game folder, or None."""
    folder = Path(folder)
    sfo_path = folder / 'sce_sys' / 'param.sfo'
    if not (folder / 'eboot.bin').is_file() or not sfo_path.is_file():
        return None
    try:
        from prepare import sfo
        values = sfo(sfo_path.read_bytes())
    except (OSError, ValueError, ImportError):
        values = {}
    icon = folder / 'sce_sys' / 'icon0.png'
    return {
        'path': str(folder),
        'id': values.get('TITLE_ID', folder.name),
        # Undecodable bytes of a title (replacement characters) are dropped.
        'title': str(values.get('TITLE', folder.name)).replace('�', '').strip(),
        'version': values.get('APP_VER', '?'),
        'icon': str(icon) if icon.is_file() else '',
    }


def library_games(folders, depth=3):
    """Games in the given library folders and below (shadPS4's "game folders"), each once."""
    games, seen = [], set()

    def visit(path, level):
        try:
            entries = list(os.scandir(path))
        except OSError:
            return
        record = game_record(path)
        if record:
            key = os.path.normcase(os.path.abspath(record['path']))
            if key not in seen:
                seen.add(key)
                games.append(record)
            return
        if level >= depth:
            return
        for entry in entries:
            if entry.is_dir(follow_symlinks=False) and entry.name.lower() not in SKIP:
                visit(entry.path, level + 1)

    for folder in folders:
        if folder and Path(folder).is_dir():
            visit(folder, 0)
    return sorted(games, key=lambda g: (g['id'] != GAME_ID, g['title'].lower()))


def local_drives():
    """Fixed drive roots (no network, CD or removable media: too slow to walk)."""
    roots = []
    try:
        mask = ctypes.windll.kernel32.GetLogicalDrives()
        get_type = ctypes.windll.kernel32.GetDriveTypeW
    except (AttributeError, OSError):
        return [Path.home().anchor]
    for i, letter in enumerate(string.ascii_uppercase):
        if mask & (1 << i):
            root = f'{letter}:\\'
            if get_type(root) == 3:  # DRIVE_FIXED
                roots.append(root)
    return roots


def scan(seconds=25.0, depth=5, extra_roots=()):
    """Walks the drives (the likely places first) for the game, sound modules and saves.
    Returns {'games': [records of GAME_ID], 'sys_modules': [folders], 'saves': [folders]}."""
    found = {'games': [], 'sys_modules': [], 'saves': []}
    seen = set()
    deadline = time.monotonic() + seconds
    home = Path.home()
    appdata = Path(os.environ.get('APPDATA', home))
    likely = [*extra_roots, appdata / 'shadPS4', home / 'Desktop', home / 'Downloads', home / 'Documents',
              home / 'Games', home]
    roots = [str(p) for p in likely if Path(p).is_dir()] + local_drives()

    def visit(path, level):
        if time.monotonic() > deadline:
            return
        key = os.path.normcase(path)
        if key in seen:
            return
        seen.add(key)
        try:
            entries = list(os.scandir(path))
        except OSError:
            return
        names = {e.name.lower(): e for e in entries}
        if 'eboot.bin' in names:
            record = game_record(path)
            if record and record['id'] == GAME_ID:
                found['games'].append(record)
            return
        if all(m.lower() in names for m in SYS_MODULES):
            found['sys_modules'].append(path)
        base = os.path.basename(path).lower()
        if base == 'savedata':
            for sub in (Path(path) / GAME_ID, *Path(path).glob('*/' + GAME_ID)):
                if sub.is_dir() and any(sub.iterdir()):
                    found['saves'].append(str(sub))
        if level >= depth:
            return
        for entry in entries:
            if time.monotonic() > deadline:
                return
            name = entry.name.lower()
            if name in SKIP or name.startswith('.'):
                continue
            try:
                if entry.is_dir(follow_symlinks=False):
                    visit(entry.path, level + 1)
            except OSError:
                pass

    for root in roots:
        visit(root, 0)
    # shadPS4's own sys_modules folders first.
    found['sys_modules'].sort(key=lambda p: (os.path.basename(p).lower() != 'sys_modules',
                                             'shadps4' not in p.lower()))
    return found
