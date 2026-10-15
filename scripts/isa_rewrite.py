# SPDX-License-Identifier: GPL-2.0-or-later
"""Find the instructions a host may lack (MOVBE, BMI1, LZCNT, SSE4a; F16C) in the linked image
and write the rewrite table the loader applies (src/isa_rewrite.h, ADR-0010, ADR-0017).

How it works, in three parts that check one another:
  1. objdump (binutils) only tells where each instruction starts and its mnemonic, over the
     function ranges of code-ranges.json (scripts/eh_frame.py); every byte outside them is
     replaced by NOP before disassembling, so data in .text is never decoded and the sweep
     resynchronizes at each function start.
  2. The operands of the ~12 rewritable encodings are decoded here, from the bytes.
  3. The mnemonic objdump printed and the encoding decoded here must agree; if not, or if the
     encoding is not one the stubs emulate, the site is UNSUPPORTED (a hard error at load time
     when the host needs that set).

Sites of 5+ bytes are patched with a jmp to a stub (JMP); sites of 4 bytes either carry the plain
next instruction into their stub (JMP_RELOCATE) or are trapped (INT3).

Usage: isa_rewrite.py out/boot-linked.bin --out out [--ranges out/code-ranges.json]
Writes <out>/isa-rewrite.bin (ISATAB1).
"""
import argparse
from bisect import bisect_right
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

# Kind numbers: the table format, src/isa_emulate.h.
(KIND_MOVBE_LOAD, KIND_MOVBE_STORE, KIND_LZCNT, KIND_TZCNT, KIND_ANDN, KIND_BEXTR, KIND_BLSI,
 KIND_BLSMSK, KIND_BLSR, KIND_VCVTPH2PS, KIND_VCVTPS2PH, KIND_EXTRQ, KIND_INSERTQ) = range(1, 14)
MODE_JMP, MODE_JMP_RELOCATE, MODE_INT3, MODE_UNSUPPORTED = range(4)
NO_REG = 0xFF
MEM_NONE, MEM_DST, MEM_SRC = 0, 1, 2
FLAG_RIP, FLAG_IMM = 1, 2
MAGIC = b'ISATAB1\0'
FLAG_RANGES_MISSING = 1

HEADER = struct.Struct('<8sQ32sIIII')                 # magic, size, sha256, sites, unsupported, flags, 0
RECORD = struct.Struct('<QBBBBBBBBBBBB4xq15sB15sx')   # mirrors IsaSite
assert HEADER.size == 64 and RECORD.size == 64

SET_OF_KIND = {KIND_MOVBE_LOAD: 'MOVBE', KIND_MOVBE_STORE: 'MOVBE', KIND_LZCNT: 'LZCNT',
               KIND_TZCNT: 'BMI1', KIND_ANDN: 'BMI1', KIND_BEXTR: 'BMI1', KIND_BLSI: 'BMI1',
               KIND_BLSMSK: 'BMI1', KIND_BLSR: 'BMI1', KIND_VCVTPH2PS: 'F16C',
               KIND_VCVTPS2PH: 'F16C', KIND_EXTRQ: 'SSE4a', KIND_INSERTQ: 'SSE4a'}
MNEMONIC_OF_KIND = {KIND_MOVBE_LOAD: 'movbe', KIND_MOVBE_STORE: 'movbe', KIND_LZCNT: 'lzcnt',
                    KIND_TZCNT: 'tzcnt', KIND_ANDN: 'andn', KIND_BEXTR: 'bextr', KIND_BLSI: 'blsi',
                    KIND_BLSMSK: 'blsmsk', KIND_BLSR: 'blsr', KIND_VCVTPH2PS: 'vcvtph2ps',
                    KIND_VCVTPS2PH: 'vcvtps2ph', KIND_EXTRQ: 'extrq', KIND_INSERTQ: 'insertq'}
KIND_OF_MNEMONIC = {}
for _kind, _name in MNEMONIC_OF_KIND.items():
    KIND_OF_MNEMONIC.setdefault(_name, _kind)


class Site:
    """One table record. Field meanings: src/isa_emulate.h (IsaSite)."""
    __slots__ = ('offset', 'length', 'kind', 'width', 'mode', 'dst', 'src1', 'src2', 'mem_slot', 'base',
                 'index', 'scale', 'mem_flags', 'disp', 'original', 'relocated', 'reason')

    def __init__(self, offset, length, kind, original):
        self.offset, self.length, self.kind, self.original = offset, length, kind, bytes(original)
        self.width = 0
        self.mode = MODE_JMP
        self.dst = self.src1 = self.src2 = self.base = self.index = NO_REG
        self.mem_slot = MEM_NONE
        self.scale = self.mem_flags = self.disp = 0
        self.relocated = b''
        self.reason = ''

    def unsupported(self, reason):
        self.mode, self.reason = MODE_UNSUPPORTED, reason
        return self


# ---- decoding the operands of the rewritable encodings, from the bytes ----

_LEGACY_PREFIXES = {0x66: 'opsize', 0x67: 'addr32', 0xF0: 'lock', 0xF2: 'repne', 0xF3: 'rep',
                    0x26: 'segment', 0x2E: 'segment', 0x36: 'segment', 0x3E: 'segment',
                    0x64: 'segment', 0x65: 'segment'}


class _Cursor:
    """Byte reader over one instruction; running past the end raises IndexError."""

    def __init__(self, data):
        self.data, self.pos = data, 0

    def peek(self):
        return self.data[self.pos] if self.pos < len(self.data) else None

    def take(self):
        if self.pos >= len(self.data):
            raise IndexError('instruction shorter than its encoding')
        self.pos += 1
        return self.data[self.pos - 1]

    def signed(self, count):
        if self.pos + count > len(self.data):
            raise IndexError('instruction shorter than its encoding')
        value = int.from_bytes(self.data[self.pos:self.pos + count], 'little', signed=True)
        self.pos += count
        return value


def _modrm(cur, rex_x, rex_b):
    """(mod, reg, rm, memory): memory = (base, index, scale, disp, rip_relative) or None for a
    register operand. reg and rm are the 3-bit fields; the caller adds REX.R / REX.B."""
    value = cur.take()
    mod, reg, rm = value >> 6, value >> 3 & 7, value & 7
    if mod == 3:
        return mod, reg, rm, None
    base, index, scale, disp, rip = NO_REG, NO_REG, 1, 0, False
    wants_disp32 = False
    if rm == 4:
        sib = cur.take()
        sib_index = (sib >> 3 & 7) | rex_x << 3
        if sib_index != 4:
            index, scale = sib_index, 1 << (sib >> 6)
        if sib & 7 == 5 and mod == 0:
            wants_disp32 = True
        else:
            base = (sib & 7) | rex_b << 3
    elif rm == 5 and mod == 0:
        rip, wants_disp32 = True, True
    else:
        base = rm | rex_b << 3
    if mod == 1:
        disp = cur.signed(1)
    elif mod == 2 or wants_disp32:
        disp = cur.signed(4)
    return mod, reg, rm, (base, index, scale, disp, rip)


def _apply_memory(site, memory, slot, ip):
    base, index, scale, disp, rip = memory
    site.mem_slot = slot
    if rip:   # the table stores the absolute address (an offset into the image)
        site.mem_flags, site.disp = FLAG_RIP, ip + site.length + disp
        site.base, site.index, site.scale = NO_REG, NO_REG, 1
    else:
        site.base, site.index, site.scale, site.disp = base, index, scale, disp


def _check_prefixes(site, prefixes, memory):
    if 'segment' in prefixes:
        return site.unsupported('segment override')
    if 'lock' in prefixes:
        return site.unsupported('lock prefix')
    if 'addr32' in prefixes and memory:
        return site.unsupported('32-bit addressing')
    return site


def decode(data, ip):
    """Site for the instruction `data` at address ip, or None when it is not one this tool
    rewrites. An encoding it recognizes but the stubs do not emulate comes back UNSUPPORTED."""
    cur = _Cursor(data)
    prefixes = set()
    while cur.peek() in _LEGACY_PREFIXES:
        prefixes.add(_LEGACY_PREFIXES[cur.take()])
    rex = 0
    if cur.peek() is not None and 0x40 <= cur.peek() <= 0x4F:
        rex = cur.take()
    try:
        if cur.peek() == 0xC4:
            return _decode_vex(cur, data, ip, prefixes)
        if cur.peek() == 0x0F:
            return _decode_legacy(cur, data, ip, prefixes, rex)
    except IndexError:
        return None
    return None


def _decode_legacy(cur, data, ip, prefixes, rex):
    cur.take()                       # 0x0F
    opcode = cur.take()
    rex_r, rex_x, rex_b, rex_w = rex >> 2 & 1, rex >> 1 & 1, rex & 1, rex >> 3 & 1
    width = 16 if 'opsize' in prefixes else 64 if rex_w else 32
    if opcode == 0x38:               # 0F 38 F0 / F1: MOVBE (with F2 they would be CRC32)
        third = cur.take()
        if third not in (0xF0, 0xF1) or 'rep' in prefixes or 'repne' in prefixes:
            return None
        site = Site(ip, len(data), KIND_MOVBE_LOAD if third == 0xF0 else KIND_MOVBE_STORE, data)
        site.width = width
        return _finish_gpr(site, cur, prefixes, ip, rex_r, rex_x, rex_b)
    if opcode in (0xBC, 0xBD) and 'rep' in prefixes:   # F3 0F BC TZCNT, F3 0F BD LZCNT
        site = Site(ip, len(data), KIND_TZCNT if opcode == 0xBC else KIND_LZCNT, data)
        site.width = width
        return _finish_gpr(site, cur, prefixes, ip, rex_r, rex_x, rex_b)
    if opcode in (0x78, 0x79) and ('opsize' in prefixes) != ('repne' in prefixes):
        return _decode_sse4a(cur, data, ip, prefixes, opcode, rex_r, rex_x, rex_b)
    return None


def _finish_gpr(site, cur, prefixes, ip, rex_r, rex_x, rex_b):
    """MOVBE/LZCNT/TZCNT: reg is the register operand, r/m the register or the memory one."""
    _, reg, rm, memory = _modrm(cur, rex_x, rex_b)
    register = reg | rex_r << 3
    if site.kind == KIND_MOVBE_STORE:         # memory destination, register source
        if not memory:
            return site.unsupported('register form of movbe')
        site.src1 = register
        _apply_memory(site, memory, MEM_DST, ip)
    else:                                     # register destination, r/m source
        site.dst = register
        if memory:
            _apply_memory(site, memory, MEM_SRC, ip)
        elif site.kind == KIND_MOVBE_LOAD:
            return site.unsupported('register form of movbe')
        else:
            site.src1 = rm | rex_b << 3
    return _check_prefixes(site, prefixes, memory)


def _decode_sse4a(cur, data, ip, prefixes, opcode, rex_r, rex_x, rex_b):
    """EXTRQ (66 0F 78 /0 ib ib; 66 0F 79 /r) and INSERTQ (F2 0F 78 /r ib ib; F2 0F 79 /r).
    The immediates are the field length, then the bit index."""
    kind = KIND_EXTRQ if 'opsize' in prefixes else KIND_INSERTQ
    site = Site(ip, len(data), kind, data)
    _, reg, rm, memory = _modrm(cur, rex_x, rex_b)
    if memory:
        return site.unsupported('memory form of an SSE4a instruction')
    xmm_reg, xmm_rm = reg | rex_r << 3, rm | rex_b << 3
    if kind == KIND_EXTRQ and opcode == 0x78:
        if reg != 0:
            return None
        site.dst = xmm_rm
    else:
        site.dst, site.src1 = xmm_reg, xmm_rm
    if opcode == 0x78:
        site.mem_flags = FLAG_IMM
        site.disp = cur.take() | cur.take() << 8
    return _check_prefixes(site, prefixes, None)


def _vex_kind(opcode, reg):
    """Kind of a 0F38 pp=0 VEX instruction: ANDN F2, BEXTR F7, and the BLS group F3 by /reg."""
    if opcode == 0xF3:                   # BLSR /1, BLSMSK /2, BLSI /3
        return {1: KIND_BLSR, 2: KIND_BLSMSK, 3: KIND_BLSI}.get(reg)
    return KIND_ANDN if opcode == 0xF2 else KIND_BEXTR


def _vex_operands(site, reg, rm_register, memory, vvvv, ip):
    """Operand slots by Intel order: BLSx dst(vvvv), src(r/m); ANDN dst(reg), src1(vvvv),
    src2(r/m); BEXTR dst(reg), src(r/m), control(vvvv). A memory r/m leaves its slot empty."""
    if site.kind in (KIND_BLSR, KIND_BLSMSK, KIND_BLSI):
        site.dst, slot = vvvv, 'src1'
    elif site.kind == KIND_ANDN:
        site.dst, site.src1, slot = reg, vvvv, 'src2'
    else:
        site.dst, site.src2, slot = reg, vvvv, 'src1'
    if memory:
        _apply_memory(site, memory, MEM_SRC, ip)
    else:
        setattr(site, slot, rm_register)


def _decode_vex(cur, data, ip, prefixes):
    """C4 VEX forms: ANDN (F2), BEXTR (F7) and the BLS group (F3), all 0F38 with pp=0; and the
    F16C pair, recognized and reported UNSUPPORTED (no stub)."""
    cur.take()                           # 0xC4
    byte1, byte2, opcode = cur.take(), cur.take(), cur.take()
    mapping, pp = byte1 & 0x1F, byte2 & 3
    f16c = {(2, 0x13): KIND_VCVTPH2PS, (3, 0x1D): KIND_VCVTPS2PH}.get((mapping, opcode)) if pp == 1 else None
    if f16c:
        return Site(ip, len(data), f16c, data).unsupported('F16C has no stub')
    if mapping != 2 or pp != 0 or byte2 >> 2 & 1 or opcode not in (0xF2, 0xF3, 0xF7):   # 128-bit only
        return None
    rex_r, rex_x, rex_b = (~byte1 >> 7) & 1, (~byte1 >> 6) & 1, (~byte1 >> 5) & 1
    _, reg, rm, memory = _modrm(cur, rex_x, rex_b)
    kind = _vex_kind(opcode, reg)
    if kind is None:
        return None
    site = Site(ip, len(data), kind, data)
    site.width = 64 if byte2 >> 7 else 32
    _vex_operands(site, reg | rex_r << 3, rm | rex_b << 3, memory, (~byte2 >> 3) & 0xF, ip)
    return _check_prefixes(site, prefixes, memory)


# ---- objdump: instruction boundaries and mnemonics ----

_LINE = re.compile(r'^\s*([0-9a-f]+):\t(\S+)\s*(.*)$')
_PREFIX_WORDS = {'rep', 'repz', 'repnz', 'repe', 'repne', 'lock', 'data16', 'addr32', 'cs', 'ds', 'es', 'fs',
                 'gs', 'ss', 'notrack', 'bnd'}
_FLOW = {'call', 'callq', 'ret', 'retq', 'retf', 'iret', 'iretq', 'int', 'int3', 'into', 'syscall', 'sysret',
         'sysenter', 'sysexit', 'ud2', 'hlt', 'loop', 'loope', 'loopne', 'xabort', 'xbegin'}
_TARGET = re.compile(r'^(?:0x)?([0-9a-f]+)$')


def objdump_instructions(path, address, objdump='objdump'):
    """Yield (address, mnemonic, has_rep_prefix, operands) from objdump -D over a raw binary
    placed at `address`. Prefix words are folded into the flags; '(bad)' is kept."""
    command = [objdump, '-D', '-b', 'binary', '-m', 'i386:x86-64', '--no-show-raw-insn',
               f'--adjust-vma={address:#x}', str(path)]
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                          bufsize=1 << 20) as process:
        for line in process.stdout:
            match = _LINE.match(line)
            if not match:
                continue
            word, rest, has_rep = match.group(2), match.group(3), False
            while word in _PREFIX_WORDS or word.startswith('rex'):
                has_rep = has_rep or word.startswith('rep')
                parts = rest.split(None, 1)
                if not parts:
                    break
                word, rest = parts[0], parts[1] if len(parts) > 1 else ''
            yield int(match.group(1), 16), word, has_rep, rest.strip()
    if process.returncode:
        raise RuntimeError(f'{objdump} failed with status {process.returncode}')


def is_flow(mnemonic):
    """Branches, calls, returns and anything else that does not simply fall through."""
    return mnemonic in _FLOW or mnemonic.startswith('j')


def is_plain(mnemonic, has_rep, operands):
    """Fit to be carried into a 4-byte site's stub: straight-line and position independent."""
    if mnemonic == '(bad)' or has_rep or is_flow(mnemonic) or mnemonic in KIND_OF_MNEMONIC:
        return False
    return '%rip' not in operands


# ---- scanning a segment ----

def mask_code(code, address, ranges):
    """The segment with every byte outside the function ranges replaced by NOP (0x90)."""
    if ranges is None:
        return bytes(code)
    masked = bytearray(b'\x90' * len(code))
    for start, size in ranges:
        low, high = max(start, address), min(start + size, address + len(code))
        if low < high:
            masked[low - address:high - address] = code[low - address:high - address]
    return bytes(masked)


def _in_ranges(ranges, address):
    if ranges is None:
        return True
    low, high = 0, len(ranges)
    while low < high:
        mid = (low + high) // 2
        if ranges[mid][0] + ranges[mid][1] <= address:
            low = mid + 1
        else:
            high = mid
    return low < len(ranges) and ranges[low][0] <= address


def _make_site(code, address, addr, length, mnemonic):
    """Site for one instruction objdump named as rewritable, cross-checked against the bytes."""
    data = code[addr - address:addr - address + length]
    site = decode(data, addr)
    if site is None:
        return Site(addr, length, KIND_OF_MNEMONIC[mnemonic], data).unsupported('encoding not decoded')
    if site.mode != MODE_UNSUPPORTED and MNEMONIC_OF_KIND[site.kind] != mnemonic:
        return site.unsupported(f'objdump says {mnemonic}, bytes say {MNEMONIC_OF_KIND[site.kind]}')
    if length < 4:
        return site.unsupported('shorter than a patch')
    return site


class ScanState:
    """What the passes over a segment accumulate: the sites, the direct branch targets (from every
    pass: a jump from a function into unwind-less code matters too) and, for each 4-byte site,
    the plain instruction after it (a candidate to be carried into the stub)."""

    def __init__(self):
        self.sites, self.targets, self.candidates = [], set(), []


def scan_into(state, instructions, code, address, accept, end=None):
    """One pass over (address, mnemonic, has_rep, operands) into `state`. The length of an
    instruction is the distance to the next one, so the stream is read one step behind; the last
    one ends at `end` (default: the end of the segment). accept(addr) says whether an address may
    hold a site: inside the function ranges, or inside a gap that decoded clean."""
    end = address + len(code) if end is None else end
    pending = None            # the 4-byte site waiting to see whether the next instruction is plain
    previous = None

    def handle(addr, length, mnemonic, has_rep, operands):
        nonlocal pending
        if is_flow(mnemonic):
            found = _TARGET.match(operands)
            if found:
                state.targets.add(int(found.group(1), 16))
        if pending is not None:
            if is_plain(mnemonic, has_rep, operands) and addr + length <= end and accept(addr):
                state.candidates.append((pending, code[addr - address:addr - address + length]))
            pending = None
        if mnemonic in KIND_OF_MNEMONIC and accept(addr):
            site = _make_site(code, address, addr, length, mnemonic)
            state.sites.append(site)
            if site.mode == MODE_JMP and length == 4:
                site.mode = MODE_INT3
                pending = site

    for item in instructions:
        if previous is not None:
            handle(previous[0], item[0] - previous[0], *previous[1:])
        previous = item
    if previous is not None:
        handle(previous[0], end - previous[0], *previous[1:])


def finalize(state):
    """Sites in address order, with the 4-byte ones whose next instruction nothing jumps to
    promoted from INT3 to JMP_RELOCATE."""
    for site, relocated in state.candidates:
        if site.offset + 4 not in state.targets:   # nothing jumps to the instruction the jmp swallows
            site.mode, site.relocated = MODE_JMP_RELOCATE, relocated
    return sorted(state.sites, key=lambda s: s.offset)


def scan_instructions(instructions, code, address, ranges):
    """Single-pass scan of a stream (the function ranges only, or everything when ranges is None)."""
    state = ScanState()
    scan_into(state, instructions, code, address, lambda addr: _in_ranges(ranges, addr))
    return finalize(state)


def gaps_between(ranges, address, size, minimum=4):
    """[(start, end)] of the stretches of the segment that no function range covers."""
    gaps, cursor, limit = [], address, address + size
    for start, length in ranges:
        if start > cursor and start - cursor >= minimum:
            gaps.append((cursor, min(start, limit)))
        cursor = max(cursor, start + length)
    if limit - cursor >= minimum:
        gaps.append((cursor, limit))
    return [(low, high) for low, high in gaps if low < limit and high > address]


def scan_clean_gaps(state, stream, code, address, gaps):
    """Second pass: code with no unwind entry (hand-written assembly, thunks) sits between the
    function ranges, and so does data. A gap whose decoding has no '(bad)' is taken as code and
    its sites join the table; one with any '(bad)' is data and stays out. `stream` holds the
    instructions of the gaps only (everything else was NOP-filled); returns the clean gaps."""
    starts = [low for low, _ in gaps]
    groups = {}
    for item in stream:
        index = bisect_right(starts, item[0]) - 1
        if index >= 0 and item[0] < gaps[index][1]:
            groups.setdefault(index, []).append(item)
    clean = []
    for index, items in sorted(groups.items()):
        if any(mnemonic == '(bad)' for _, mnemonic, _, _ in items):
            continue
        low, high = gaps[index]
        before = len(state.sites)
        scan_into(state, items, code, address, lambda addr: True, end=high)
        for site in state.sites[before:]:
            site.reason = 'unwind-less code'
        clean.append((low, high))
    return clean


def _disassemble(path, data, address, objdump):
    path.write_bytes(data)
    try:
        yield from objdump_instructions(path, address, objdump)
    finally:
        path.unlink(missing_ok=True)


def scan_segment(code, address, ranges, workdir, objdump='objdump'):
    """Sites of one executable segment loaded at `address`, in address order.

    ranges: sorted [(start, size)] absolute, or None for a linear scan of everything. With
    ranges, pass 1 disassembles only them (the rest NOP-filled) and pass 2 the clean gaps."""
    path = Path(workdir) / 'isa-segment.tmp'
    state = ScanState()
    scan_into(state, _disassemble(path, mask_code(code, address, ranges), address, objdump), code, address,
              lambda addr: _in_ranges(ranges, addr))
    if ranges is not None:
        gaps = gaps_between(ranges, address, len(code))
        if gaps:
            gap_ranges = [(low, high - low) for low, high in gaps]
            stream = _disassemble(path, mask_code(code, address, gap_ranges), address, objdump)
            scan_clean_gaps(state, stream, code, address, gaps)
    return finalize(state)


# ---- the linked image, the ranges and the table ----

def read_boot_linked(data):
    """(segments, image) of a BBPROBE5 file: segments are (address, size, flags)."""
    magic, size, _entry, nsegments, nrelocs, nnames, _flags = struct.unpack_from('<8s6Q', data, 0)
    if magic != b'BBPROBE5':
        raise ValueError('not a BBPROBE5 image')
    pos = 56 + 8 + 32                      # header, procparam, main TLS
    nmodules, = struct.unpack_from('<Q', data, pos)
    pos += 8 + nmodules * 56
    nbindings, = struct.unpack_from('<Q', data, pos)
    pos += 8 + nbindings * 24
    segments = [struct.unpack_from('<3Q', data, pos + i * 24) for i in range(nsegments)]
    pos += nsegments * 24 + nnames * 128 + nrelocs * 32
    if pos + size != len(data):
        raise ValueError('unexpected boot trailer')
    return segments, data[pos:pos + size]


def segment_ranges(modules, address):
    """Sorted ranges of the module that holds `address`, or None when it has none (linear scan)."""
    for module in modules or ():
        if module['base'] <= address < module['base'] + module['size']:
            if module.get('ranges') is None:
                return None
            return sorted((start, size) for start, size in module['ranges'])
    return None


def encode_table(image, sites, flags=0):
    """ISATAB1: header, then one record per site in address order."""
    ordered = sorted(sites, key=lambda s: s.offset)
    unsupported = sum(s.mode == MODE_UNSUPPORTED for s in ordered)
    parts = [HEADER.pack(MAGIC, len(image), hashlib.sha256(image).digest(), len(ordered), unsupported, flags, 0)]
    for s in ordered:
        parts.append(RECORD.pack(s.offset, s.length, s.kind, s.width, s.mode, s.dst, s.src1, s.src2, s.mem_slot,
                                 s.base, s.index, s.scale, s.mem_flags, s.disp, s.original.ljust(15, b'\0'),
                                 len(s.relocated), s.relocated.ljust(15, b'\0')))
    return b''.join(parts)


def summary(sites, flags=0):
    sets = [SET_OF_KIND[s.kind] for s in sites]
    modes = [s.mode for s in sites]
    per_set = ', '.join(f'{name} {sets.count(name)}' for name in ('MOVBE', 'BMI1', 'LZCNT', 'SSE4a', 'F16C'))
    text = (f'ISA: {len(sites)} sites ({per_set}); {modes.count(MODE_JMP_RELOCATE)} relocated, '
            f'{modes.count(MODE_INT3)} int3, {modes.count(MODE_UNSUPPORTED)} unsupported')
    gap_sites = sum(1 for s in sites if s.reason == 'unwind-less code')
    if gap_sites:
        text += f'; {gap_sites} in code without unwind tables'
    if flags & FLAG_RANGES_MISSING:
        text += '; linear scan of modules without unwind tables'
    return text


def table_matches(path, image):
    """True when the table at path was generated from exactly this image (it is then reused:
    the scan takes about a minute for the game and run.sh builds the image on every launch)."""
    try:
        head = Path(path).read_bytes()[:HEADER.size]
    except OSError:
        return False
    if len(head) < HEADER.size:
        return False
    magic, size, digest = HEADER.unpack(head)[:3]
    return magic == MAGIC and size == len(image) and digest == hashlib.sha256(image).digest()


def generate(boot, out, ranges_path=None, objdump='objdump'):
    """Scan the executable segments of boot (a BBPROBE5 file) and write <out>/isa-rewrite.bin.
    Returns (sites, flags), or (None, 0) when an existing table already matches the image."""
    segments, image = read_boot_linked(Path(boot).read_bytes())
    if table_matches(Path(out, 'isa-rewrite.bin'), image):
        return None, 0
    modules = None
    if ranges_path and Path(ranges_path).exists():
        modules = json.loads(Path(ranges_path).read_text(encoding='utf-8')).get('modules')
    sites, flags = [], 0
    for address, size, segment_flags in segments:
        if not segment_flags & 1:
            continue
        ranges = segment_ranges(modules, address)
        if ranges is None:
            flags |= FLAG_RANGES_MISSING
        code = image[address:address + size]
        sites += scan_segment(code, address, ranges, out, objdump)
    Path(out, 'isa-rewrite.bin').write_bytes(encode_table(image, sites, flags))
    return sites, flags


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('boot', type=Path)
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'out')
    parser.add_argument('--ranges', type=Path, help='code-ranges.json (default: beside the boot file)')
    parser.add_argument('--objdump', default='objdump')
    args = parser.parse_args(argv)
    try:
        sites, flags = generate(args.boot, args.out, args.ranges or args.boot.parent / 'code-ranges.json',
                                args.objdump)
    except (OSError, ValueError, RuntimeError) as error:
        print(f'ISA: rewrite table unavailable ({error})')
        return 1
    print('ISA: rewrite table reused (same image)' if sites is None else summary(sites, flags))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
