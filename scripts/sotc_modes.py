#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""sotc: optional game modes for Shadow of the Colossus EU 1.01 (CUSA08809), as out/patches.bin.

    python scripts/sotc_modes.py --out out --modes af16,fps60

The modes are the shadPS4 SotC fork's "SotC-Modes" patches (GoldHEN addresses, image base
0x400000), written in the loader's BBPATCH2 format (bb-probe --patches):
  af16   16x anisotropic filtering on every texture (cvars appended to the command line)
  fps60  removes the game's 30 FPS cap (illusion's GoldHEN patch)
The original bytes are checked first: another game version gets no patch at all.
"""
import argparse
from pathlib import Path
import struct
from prepare import parse_self

BASE = 0x400000
AF16_CVARS = (b'+progSamplerAnisoLevel=16 +progSamplerAnisoLevel_BasePS4=16 +progSamplerAnisoLevel_Perf=16 '
              b'+progSamplerAnisoLevel_Cinematic=16 +linearSamplerAnisoLevel=16')
# mode: [(address, expected original bytes, new bytes)]
MODES = {
    'af16': [
        # lea rsi, [the startup command line] -> the cvars written below
        (0x009367b9, bytes.fromhex('488d352debad00'), bytes.fromhex('488d35b574ac00')),
        # over an unused debug path string
        (0x013fdc75, b'd:\\dev\\cp10\\Main/Code/BPE/Source\\Core/Th', AF16_CVARS),
        (0x013fdd0f, b'E', b'\0'),
    ],
    'fps60': [
        # the frame limiter's import stub returns 0
        (0x01390090, bytes.fromhex('ff25a20567'), bytes.fromhex('31c0c3')),
    ],
}


def image_bytes(elf, ph, vaddr, length):
    for p in ph:
        if p['type'] in (1, 0x61000010) and p['vaddr'] <= vaddr and vaddr + length <= p['vaddr'] + p['filesz']:
            offset = p['offset'] + vaddr - p['vaddr']
            return bytes(elf[offset:offset + length])
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'out')
    parser.add_argument('--modes', default='', help='comma-separated: ' + ', '.join(MODES))
    args = parser.parse_args()
    elf, _, ph, _, _ = parse_self((args.game / 'eboot.bin').read_bytes())
    writes = []
    for mode in filter(None, (m.strip().lower() for m in args.modes.split(','))):
        if mode not in MODES:
            raise SystemExit(f'unknown mode {mode!r} (known: {", ".join(MODES)})')
        lines = MODES[mode]
        if any(image_bytes(elf, ph, address - BASE, len(original)) != original for address, original, _ in lines):
            print(f'Mode {mode}: this eboot is not EU 1.01, not applied')
            continue
        writes += [(address - BASE, data) for address, _, data in lines]
        print(f'Mode {mode}: on')
    blob = struct.pack('<8sQQ', b'BBPATCH2', BASE, len(writes))
    for offset, data in writes:
        blob += struct.pack('<QQ', offset, len(data)) + data
    (args.out / 'patches.bin').write_bytes(blob)


if __name__ == '__main__':
    main()
