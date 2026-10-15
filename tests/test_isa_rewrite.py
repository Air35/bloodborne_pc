# SPDX-License-Identifier: GPL-2.0-or-later
"""The ISA rewrite table generator (scripts/isa_rewrite.py, ADR-0017).

Everything is synthetic code assembled here, byte by byte. The decoder is checked against the
Intel/AMD encodings; the scanner against an instruction stream written by hand (hermetic) and,
when objdump exists, against the real objdump over the same bytes.
"""
import paths  # noqa: F401  (puts scripts/ on sys.path)
import hashlib
import os
import shutil
import struct
import tempfile
import unittest
from pathlib import Path

import isa_rewrite as rw

BASE = 0x1000


def site(hex_bytes, ip=BASE):
    return rw.decode(bytes.fromhex(hex_bytes), ip)


class DecodeTests(unittest.TestCase):
    def test_movbe_load_with_sib_and_rex(self):
        # movbe r13d? 44 0f 38 f0 6c 8b 04: REX.R, [rbx + rcx*4 + 4] -> r13d
        s = site('440f38f06c8b04')
        self.assertEqual((s.kind, s.width, s.dst, s.mem_slot, s.base, s.index, s.scale, s.disp),
                         (rw.KIND_MOVBE_LOAD, 32, 13, rw.MEM_SRC, 3, 1, 4, 4))

    def test_movbe_store_64_bit(self):
        s = site('480f38f10b')                  # movbe [rbx], rcx
        self.assertEqual((s.kind, s.width, s.src1, s.dst, s.mem_slot, s.base), (rw.KIND_MOVBE_STORE, 64, 1, rw.NO_REG, rw.MEM_DST, 3))

    def test_movbe_16_bit_uses_the_operand_size_prefix(self):
        self.assertEqual(site('660f38f008').width, 16)

    def test_movbe_register_form_does_not_exist(self):
        self.assertEqual(site('0f38f0c1').mode, rw.MODE_UNSUPPORTED)

    def test_crc32_is_not_movbe(self):
        self.assertIsNone(site('f20f38f0c1'))   # crc32 r32, r/m8

    def test_lzcnt_and_tzcnt_register_and_memory(self):
        s = site('f30fbdc1')                    # lzcnt eax, ecx
        self.assertEqual((s.kind, s.width, s.dst, s.src1, s.mem_slot), (rw.KIND_LZCNT, 32, 0, 1, rw.MEM_NONE))
        s = site('f3480fbc4c2410')              # tzcnt rcx, [rsp+0x10]
        self.assertEqual((s.kind, s.width, s.dst, s.src1, s.mem_slot, s.base, s.index, s.disp),
                         (rw.KIND_TZCNT, 64, 1, rw.NO_REG, rw.MEM_SRC, 4, rw.NO_REG, 0x10))

    def test_bsr_is_not_lzcnt(self):
        self.assertIsNone(site('0fbdc1'))       # bsr eax, ecx (no F3)

    def test_vex_bmi_register_forms(self):
        s = site('c4e270f3c9')                  # blsr ecx, ecx
        self.assertEqual((s.kind, s.width, s.dst, s.src1), (rw.KIND_BLSR, 32, 1, 1))
        s = site('c4e270f2c2')                  # andn eax, ecx, edx : dst=eax src1=ecx(vvvv) src2=edx(rm)
        self.assertEqual((s.kind, s.dst, s.src1, s.src2), (rw.KIND_ANDN, 0, 1, 2))
        s = site('c4e270f7c2')                  # bextr eax, edx, ecx : dst=eax src=edx(rm) control=ecx(vvvv)
        self.assertEqual((s.kind, s.dst, s.src1, s.src2), (rw.KIND_BEXTR, 0, 2, 1))

    def test_vex_bls_group_selects_by_reg_field(self):
        self.assertEqual(site('c4e270f3c9').kind, rw.KIND_BLSR)     # /1
        self.assertEqual(site('c4e270f3d1').kind, rw.KIND_BLSMSK)   # /2
        self.assertEqual(site('c4e270f3d9').kind, rw.KIND_BLSI)     # /3
        self.assertIsNone(site('c4e270f3c1'))                       # /0 is not a BMI1 instruction

    def test_vex_64_bit_and_extended_registers(self):
        # andn r10, r9, r8 : VEX.NDS.LZ.0F38.W1 F2 /r ; R̄=0 X̄=1 B̄=0 -> bytes c4 42 b0 f2 d0
        s = site('c442b0f2d0')
        self.assertEqual((s.kind, s.width, s.dst, s.src1, s.src2), (rw.KIND_ANDN, 64, 10, 9, 8))

    def test_vex_memory_operand_leaves_its_slot_empty(self):
        s = site('c4e270f20c24')                # andn ecx, ecx, [rsp]
        self.assertEqual((s.src2, s.mem_slot, s.base), (rw.NO_REG, rw.MEM_SRC, 4))

    def test_rip_relative_becomes_an_absolute_offset(self):
        s = site('0f38f005 10000000'.replace(' ', ''), ip=0x1070)   # movbe eax, [rip+0x10]
        self.assertEqual((s.mem_flags, s.disp, s.base, s.index), (rw.FLAG_RIP, 0x1070 + 8 + 0x10, rw.NO_REG, rw.NO_REG))

    def test_negative_displacement(self):
        s = site('f30fbd4dfc')                  # lzcnt ecx, [rbp-4]
        self.assertEqual((s.base, s.disp), (5, -4))

    def test_segment_lock_and_32_bit_addressing_are_unsupported(self):
        self.assertEqual(site('640f38f008').mode, rw.MODE_UNSUPPORTED)          # fs:
        self.assertEqual(site('670f38f008').mode, rw.MODE_UNSUPPORTED)          # addr32
        self.assertEqual(site('f00f38f008').mode, rw.MODE_UNSUPPORTED)          # lock

    def test_sse4a_forms(self):
        s = site('660f79c1')                    # extrq xmm0, xmm1
        self.assertEqual((s.kind, s.dst, s.src1, s.mem_flags), (rw.KIND_EXTRQ, 0, 1, 0))
        s = site('660f78c11008')                # extrq xmm1, 0x10, 0x08
        self.assertEqual((s.kind, s.dst, s.src1, s.mem_flags, s.disp), (rw.KIND_EXTRQ, 1, rw.NO_REG, rw.FLAG_IMM, 0x0810))
        s = site('f20f79c1')                    # insertq xmm0, xmm1
        self.assertEqual((s.kind, s.dst, s.src1, s.mem_flags), (rw.KIND_INSERTQ, 0, 1, 0))
        s = site('f20f78d11008')                # insertq xmm2, xmm1, 0x10, 0x08
        self.assertEqual((s.kind, s.dst, s.src1, s.mem_flags, s.disp), (rw.KIND_INSERTQ, 2, 1, rw.FLAG_IMM, 0x0810))
        s = site('66440f79c1')                  # extrq xmm8, xmm1 (REX.R)
        self.assertEqual((s.dst, s.src1), (8, 1))

    def test_sse4a_memory_form_and_other_opcodes(self):
        self.assertEqual(site('660f7908').mode, rw.MODE_UNSUPPORTED)    # mod != 3
        self.assertIsNone(site('660f78c808'))                           # 66 0F 78 /1 is not EXTRQ
        self.assertIsNone(site('0f79c1'))                               # neither prefix: VMREAD-ish, not SSE4a

    def test_f16c_is_recognized_and_unsupported(self):
        s = site('c4e27913c1')                  # vcvtph2ps xmm0, xmm1
        self.assertEqual((s.kind, s.mode), (rw.KIND_VCVTPH2PS, rw.MODE_UNSUPPORTED))
        s = site('c4e3791dc100')                # vcvtps2ph xmm1, xmm0, 0
        self.assertEqual((s.kind, s.mode), (rw.KIND_VCVTPS2PH, rw.MODE_UNSUPPORTED))

    def test_truncated_instruction_is_not_decoded(self):
        self.assertIsNone(site('0f38'))
        self.assertIsNone(site('c4e270'))


# ---- the scanner ----

# (hex bytes, objdump mnemonic, operands); the program starts at BASE and is contiguous.
PROGRAM = [
    ('0f38f008', 'movbe', '(%rax),%ecx'),                 # 0x1000  4 bytes, then a plain mov: RELOCATE
    ('89c8', 'mov', '%ecx,%eax'),                         # 0x1004
    ('c4e270f3c9', 'blsr', '%ecx,%ecx'),                  # 0x1006  JMP
    ('c3', 'ret', ''),                                    # 0x100b
    ('90' * 4, 'nop', ''),                                # 0x100c filler, 4 x 1 byte -> see EXPANDED
    ('f30fbdc1', 'lzcnt', '%ecx,%eax'),                   # 0x1010  4 bytes, next is a branch target: INT3
    ('89c1', 'mov', '%eax,%ecx'),                         # 0x1014
    ('c3', 'ret', ''),                                    # 0x1016
    ('90' * 9, 'nop', ''),                                # 0x1017 .. 0x101f
    ('e9efffffff', 'jmp', '0x1014'),                      # 0x1020 targets the instruction after the lzcnt
    ('90' * 11, 'nop', ''),                               # 0x1025 .. 0x102f
    ('640f38f008', 'movbe', '%fs:(%rax),%ecx'),           # 0x1030 segment override: UNSUPPORTED
    ('90' * 11, 'nop', ''),                               # 0x1035 .. 0x103f
    ('660f79c1', 'extrq', '%xmm1,%xmm0'),                 # 0x1040 4 bytes, plain next: RELOCATE
    ('4889c8', 'mov', '%rcx,%rax'),                       # 0x1044
    ('90' * 9, 'nop', ''),                                # 0x1047 .. 0x104f
    ('f20f78c11008', 'insertq', '$0x8,$0x10,%xmm1,%xmm0'),  # 0x1050 6 bytes: JMP
    ('90' * 10, 'nop', ''),                               # 0x1056 .. 0x105f
    ('c4e27913c1', 'vcvtph2ps', '%xmm1,%xmm0'),           # 0x1060 F16C: UNSUPPORTED
    ('90' * 11, 'nop', ''),                               # 0x1065 .. 0x106f
    ('0f38f00510000000', 'movbe', '0x10(%rip),%eax'),     # 0x1070 rip-relative, 8 bytes: JMP
    ('90' * 136, 'nop', ''),                              # 0x1078 .. 0x10ff
    ('0f38f008', 'movbe', '(%rax),%ecx'),                 # 0x1100 data, outside the function ranges
    ('06', '(bad)', ''),                                  # 0x1104 invalid in 64-bit mode: a dirty gap
    ('90' * 11, 'nop', ''),                               # 0x1105 .. 0x110f
]
FUNCTIONS = [(0x1000, 0x10), (0x1010, 0x20), (0x1030, 0x10), (0x1040, 0x40)]   # not 0x1100


def assemble():
    """(code, [(address, mnemonic, has_rep, operands)]) one entry per instruction."""
    code, stream = bytearray(), []
    for hex_bytes, mnemonic, operands in PROGRAM:
        raw = bytes.fromhex(hex_bytes)
        if mnemonic == 'nop':
            for _ in raw:                       # one 0x90 per instruction, as objdump prints them
                stream.append((BASE + len(code), 'nop', False, ''))
                code.append(0x90)
            continue
        stream.append((BASE + len(code), mnemonic, False, operands))
        code += raw
    return bytes(code), stream


def summary(sites):
    return [(s.offset, s.kind, s.mode, s.relocated.hex()) for s in sorted(sites, key=lambda s: s.offset)]


class ScanTests(unittest.TestCase):
    def setUp(self):
        self.code, self.stream = assemble()

    def scan(self, ranges):
        return rw.scan_instructions(self.stream, self.code, BASE, ranges)

    def test_modes_by_length_and_context(self):
        by_offset = {s.offset: s for s in self.scan(FUNCTIONS)}
        self.assertEqual(by_offset[0x1000].mode, rw.MODE_JMP_RELOCATE)
        self.assertEqual(by_offset[0x1000].relocated, bytes.fromhex('89c8'))
        self.assertEqual(by_offset[0x1006].mode, rw.MODE_JMP)
        self.assertEqual(by_offset[0x1010].mode, rw.MODE_INT3)          # a jmp lands on the next instruction
        self.assertEqual(by_offset[0x1010].relocated, b'')
        self.assertEqual(by_offset[0x1030].mode, rw.MODE_UNSUPPORTED)
        self.assertEqual(by_offset[0x1040].mode, rw.MODE_JMP_RELOCATE)
        self.assertEqual(by_offset[0x1050].mode, rw.MODE_JMP)
        self.assertEqual(by_offset[0x1060].mode, rw.MODE_UNSUPPORTED)
        self.assertEqual(by_offset[0x1070].mode, rw.MODE_JMP)
        self.assertEqual(by_offset[0x1070].disp, 0x1070 + 8 + 0x10)

    def test_data_outside_the_function_ranges_is_not_a_site(self):
        offsets = [s.offset for s in self.scan(FUNCTIONS)]
        self.assertNotIn(0x1100, offsets)
        # a linear scan (no unwind tables) would take it, which is what the ranges prevent
        self.assertIn(0x1100, [s.offset for s in self.scan(None)])

    def test_gaps_between_ranges(self):
        self.assertEqual(rw.gaps_between([(0x1000, 0x10), (0x1020, 0x10)], 0x1000, 0x50), [(0x1010, 0x1020), (0x1030, 0x1050)])
        self.assertEqual(rw.gaps_between([(0x1000, 0x10), (0x1010, 0x10)], 0x1000, 0x20), [])      # touching
        # a 2-byte stretch before the range is below the minimum; the 12 bytes after it are a gap
        self.assertEqual(rw.gaps_between([(0x1002, 2)], 0x1000, 0x10), [(0x1004, 0x1010)])
        self.assertEqual(rw.gaps_between([], 0x1000, 0x20), [(0x1000, 0x1020)])

    def test_clean_gap_sites_join_and_dirty_gap_sites_do_not(self):
        # [0x1000,0x1010) is a function; gap A [0x1010,0x1020) decodes clean; gap B [0x1020,0x1030) has a (bad)
        code = b'\xc3' + b'\x90' * 15 + bytes.fromhex('0f38f008') + bytes.fromhex('89c8') + b'\x90' * 10 + \
            bytes.fromhex('0f38f008') + bytes.fromhex('06') + b'\x90' * 11
        stream = [(0x1000, 'ret', False, '')] + [(0x1001 + i, 'nop', False, '') for i in range(15)]
        stream += [(0x1010, 'movbe', False, '(%rax),%ecx'), (0x1014, 'mov', False, '%ecx,%eax')]
        stream += [(0x1016 + i, 'nop', False, '') for i in range(10)]
        stream += [(0x1020, 'movbe', False, '(%rax),%ecx'), (0x1024, '(bad)', False, '')]
        stream += [(0x1025 + i, 'nop', False, '') for i in range(11)]
        gaps = [(0x1010, 0x1020), (0x1020, 0x1030)]
        state = rw.ScanState()
        clean = rw.scan_clean_gaps(state, stream, code, BASE, gaps)
        self.assertEqual(clean, [(0x1010, 0x1020)])
        sites = rw.finalize(state)
        self.assertEqual([s.offset for s in sites], [0x1010])
        self.assertEqual((sites[0].mode, sites[0].reason, sites[0].relocated.hex()),
                         (rw.MODE_JMP_RELOCATE, 'unwind-less code', '89c8'))

    def test_a_jump_from_a_function_into_a_clean_gap_blocks_the_relocation(self):
        code = bytes.fromhex('e90b000000') + b'\x90' * 11 + bytes.fromhex('0f38f008') + bytes.fromhex('89c8') + b'\x90' * 10
        stream = [(0x1000, 'jmp', False, '0x1014')] + [(0x1005 + i, 'nop', False, '') for i in range(11)]
        stream += [(0x1010, 'movbe', False, '(%rax),%ecx'), (0x1014, 'mov', False, '%ecx,%eax')]
        stream += [(0x1016 + i, 'nop', False, '') for i in range(10)]
        state = rw.ScanState()
        rw.scan_into(state, stream[:12], code, BASE, lambda addr: addr < 0x1010, end=0x1010)   # pass 1 sees the jmp
        rw.scan_clean_gaps(state, stream[12:], code, BASE, [(0x1010, 0x1020)])
        sites = rw.finalize(state)
        self.assertEqual((sites[0].offset, sites[0].mode), (0x1010, rw.MODE_INT3))

    def test_next_instruction_must_be_plain(self):
        for operands, mnemonic, expect in (('0x2000', 'jmp', rw.MODE_INT3), ('%rax', 'ret', rw.MODE_INT3),
                                           ('0x10(%rip),%eax', 'mov', rw.MODE_INT3), ('%ecx,%eax', 'mov', rw.MODE_JMP_RELOCATE)):
            code = bytes.fromhex('0f38f008') + b'\x89\xc8' + b'\x90' * 8
            stream = [(BASE, 'movbe', False, '(%rax),%ecx'), (BASE + 4, mnemonic, False, operands),
                      (BASE + 6, 'nop', False, '')]
            for i in range(7):
                stream.append((BASE + 7 + i, 'nop', False, ''))
            found = rw.scan_instructions(stream, code, BASE, None)
            self.assertEqual(found[0].mode, expect, (mnemonic, operands))

    def test_rep_prefixed_next_instruction_is_not_relocated(self):
        code = bytes.fromhex('0f38f008') + b'\xf3\xa4' + b'\x90' * 4
        stream = [(BASE, 'movbe', False, '(%rax),%ecx'), (BASE + 4, 'movsb', True, '%ds:(%rsi),%es:(%rdi)')]
        stream += [(BASE + 6 + i, 'nop', False, '') for i in range(4)]
        self.assertEqual(rw.scan_instructions(stream, code, BASE, None)[0].mode, rw.MODE_INT3)

    def test_mnemonic_and_bytes_must_agree(self):
        code = bytes.fromhex('f30fbdc1') + b'\x90' * 8                  # the bytes are lzcnt ...
        stream = [(BASE, 'tzcnt', False, '%ecx,%eax')] + [(BASE + 4 + i, 'nop', False, '') for i in range(8)]
        found = rw.scan_instructions(stream, code, BASE, None)          # ... objdump claims tzcnt
        self.assertEqual(found[0].mode, rw.MODE_UNSUPPORTED)
        self.assertIn('objdump says', found[0].reason)

    def test_undecoded_encoding_is_unsupported_not_dropped(self):
        code = bytes.fromhex('0f38f1c1') + b'\x90' * 8                  # movbe register form
        stream = [(BASE, 'movbe', False, '%ecx,%ecx')] + [(BASE + 4 + i, 'nop', False, '') for i in range(8)]
        self.assertEqual(rw.scan_instructions(stream, code, BASE, None)[0].mode, rw.MODE_UNSUPPORTED)


@unittest.skipUnless(shutil.which('objdump'), 'binutils objdump not installed')
class ObjdumpIntegrationTests(unittest.TestCase):
    """The same program through the real objdump must give the same sites as the hand-written stream."""

    def test_real_objdump_agrees_with_the_hermetic_stream(self):
        code, stream = assemble()
        hermetic = rw.scan_instructions(stream, code, BASE, FUNCTIONS)
        with tempfile.TemporaryDirectory() as work:
            real = rw.scan_segment(code, BASE, FUNCTIONS, work)
        self.assertEqual(summary(real), summary(hermetic))
        self.assertFalse(any(Path(p).name == 'isa-segment.tmp' for p in Path(work).glob('*')))

    def test_masking_replaces_everything_outside_the_ranges(self):
        code, _ = assemble()
        masked = rw.mask_code(code, BASE, FUNCTIONS)
        self.assertEqual(masked[0x100:0x104], b'\x90' * 4)               # the data movbe at 0x1100
        self.assertEqual(masked[:0x10], code[:0x10])
        self.assertEqual(len(masked), len(code))


FIXTURE = Path(__file__).resolve().parent / 'data' / 'isa_table_fixture.hex'
FIXTURE_HEADER = ('# ISATAB1 table of the synthetic program in tests/test_isa_rewrite.py (no game bytes).\n'
                  '# Written by that test (SOTC_UPDATE_FIXTURES=1) and read by tests/test_isa_rewrite.c.\n')


def fixture_text():
    """The table of the synthetic program above, as hex text: what the C loader test reads."""
    code, stream = assemble()
    sites = rw.scan_instructions(stream, code, BASE, FUNCTIONS)
    data = rw.encode_table(bytes(BASE) + code, sites, 0)
    lines = [data[i:i + 32].hex() for i in range(0, len(data), 32)]
    return FIXTURE_HEADER + '\n'.join(lines) + '\n'


class FixtureTests(unittest.TestCase):
    """The generator and the C loader agree through this file (synthetic code, no game bytes)."""

    def test_committed_fixture_is_what_the_generator_writes(self):
        text = fixture_text()
        if os.environ.get('SOTC_UPDATE_FIXTURES') == '1':
            FIXTURE.write_text(text, encoding='ascii')
        self.assertEqual(FIXTURE.read_text(encoding='ascii'), text, 'regenerate with SOTC_UPDATE_FIXTURES=1')


class TableTests(unittest.TestCase):
    def test_header_and_records_round_trip(self):
        code, stream = assemble()
        sites = rw.scan_instructions(stream, code, BASE, FUNCTIONS)
        image = bytes(BASE) + code
        data = rw.encode_table(image, sites, rw.FLAG_RANGES_MISSING)
        self.assertEqual(len(data), 64 + 64 * len(sites))
        magic, size, digest, count, unsupported, flags, zero = rw.HEADER.unpack_from(data, 0)
        self.assertEqual((magic, size, count, flags, zero), (rw.MAGIC, len(image), len(sites), 1, 0))
        self.assertEqual(digest, hashlib.sha256(image).digest())
        self.assertEqual(unsupported, sum(s.mode == rw.MODE_UNSUPPORTED for s in sites))
        offsets = [rw.RECORD.unpack_from(data, 64 + 64 * i)[0] for i in range(count)]
        self.assertEqual(offsets, sorted(offsets))
        first = rw.RECORD.unpack_from(data, 64)
        self.assertEqual((first[0], first[1], first[2], first[4]), (0x1000, 4, rw.KIND_MOVBE_LOAD, rw.MODE_JMP_RELOCATE))
        self.assertEqual(first[14][:4], bytes.fromhex('0f38f008'))      # original
        self.assertEqual(first[15], 2)                                  # relocated length

    def test_summary_text(self):
        code, stream = assemble()
        text = rw.summary(rw.scan_instructions(stream, code, BASE, FUNCTIONS), rw.FLAG_RANGES_MISSING)
        self.assertIn('ISA: 8 sites', text)      # the 9th movbe is data, outside the ranges
        self.assertIn('2 unsupported', text)
        self.assertIn('linear scan', text)

    def test_boot_linked_reader_rejects_other_formats(self):
        with self.assertRaises(ValueError):
            rw.read_boot_linked(b'BBPROBE2' + bytes(100))

    def test_segment_ranges_choose_the_module(self):
        modules = [{'base': 0, 'size': 0x1000, 'ranges': [[0x10, 4], [0x0, 8]]},
                   {'base': 0x1000, 'size': 0x1000, 'ranges': None}]
        self.assertEqual(rw.segment_ranges(modules, 0x500), [(0, 8), (0x10, 4)])
        self.assertIsNone(rw.segment_ranges(modules, 0x1800))           # module without tables
        self.assertIsNone(rw.segment_ranges(modules, 0x9000))           # no module
        self.assertIsNone(rw.segment_ranges(None, 0))


if __name__ == '__main__':
    unittest.main()
