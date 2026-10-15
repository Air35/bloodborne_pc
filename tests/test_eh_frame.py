# SPDX-License-Identifier: GPL-2.0-or-later
"""Function ranges from a synthetic .eh_frame_hdr (scripts/eh_frame.py, ADR-0017).

The unwind tables are built by hand, byte by byte, so nothing of any game is involved.
"""
import paths  # noqa: F401  (puts scripts/ on sys.path)
import struct
import unittest
import eh_frame

HEADER = 0x1000        # address of the .eh_frame_hdr in the synthetic image
FRAME = 0x2000         # address of the .eh_frame


def uleb(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def cie(augmentation=b'zR', fde_encoding=0x1B):
    """A CIE record (length included) whose FDE pointer encoding is fde_encoding."""
    body = struct.pack('<I', 0) + bytes([1]) + augmentation + b'\0' + uleb(1) + bytes([0x78]) + bytes([16])
    data = b''
    for letter in augmentation[1:]:
        if letter == ord('R'):
            data += bytes([fde_encoding])
        elif letter == ord('L'):
            data += bytes([0x1B])
        elif letter == ord('P'):
            data += bytes([0x00]) + struct.pack('<Q', 0xAAAA)  # absolute personality pointer
    body += uleb(len(data)) + data
    body += b'\0' * (-(len(body) + 4) % 4)
    return struct.pack('<I', len(body)) + body


def fde(position, start, size, absolute):
    """An FDE at `position`: CIE pointer back to FRAME, start (pcrel or absolute) and range."""
    begin = start if absolute else start - (position + 8)
    body = struct.pack('<IiI', position + 4 - FRAME, begin, size) + uleb(0)
    body += b'\0' * (-(len(body) + 4) % 4)
    return struct.pack('<I', len(body)) + body


def build(functions, augmentation=b'zR', fde_encoding=0x1B, version=1):
    """({address: bytes}, read) for a CIE, one FDE per (start, size) and the hdr table."""
    absolute = (fde_encoding & 0x70) == 0
    frame = bytearray(cie(augmentation, fde_encoding))
    entries = []
    for start, size in functions:
        position = FRAME + len(frame)
        entries.append((start, position))
        frame += fde(position, start, size, absolute)
    frame += struct.pack('<I', 0)  # terminator
    table = b''.join(struct.pack('<ii', start - HEADER, at - HEADER) for start, at in sorted(entries))
    header = bytes([version, 0x1B, 0x03, 0x3B]) + struct.pack('<i', FRAME - (HEADER + 4)) + \
        struct.pack('<I', len(entries)) + table
    memory = {HEADER: header, FRAME: bytes(frame)}

    def read(address, size):
        for origin, data in memory.items():
            if origin <= address and address + size <= origin + len(data):
                return data[address - origin:address - origin + size]
        raise eh_frame.EhFrameError(f'unmapped {address:#x}')
    return memory, read


class FunctionRangesTests(unittest.TestCase):
    def ranges(self, functions, **options):
        _, read = build(functions, **options)
        return eh_frame.function_ranges(read, HEADER)

    def test_lists_every_function_sorted(self):
        self.assertEqual(self.ranges([(0x5100, 0x40), (0x5000, 0x20), (0x6000, 0x10)]),
                         [(0x5000, 0x20), (0x5100, 0x40), (0x6000, 0x10)])

    def test_touching_and_overlapping_ranges_are_merged(self):
        self.assertEqual(self.ranges([(0x5000, 0x20), (0x5020, 0x20), (0x5030, 0x40), (0x7000, 4)]),
                         [(0x5000, 0x70), (0x7000, 4)])

    def test_zero_sized_function_is_ignored(self):
        self.assertEqual(self.ranges([(0x5000, 0), (0x5100, 8)]), [(0x5100, 8)])

    def test_personality_and_lsda_in_the_cie_are_skipped(self):
        self.assertEqual(self.ranges([(0x5000, 0x20)], augmentation=b'zPLR'), [(0x5000, 0x20)])

    def test_absolute_sdata4_encoding(self):
        self.assertEqual(self.ranges([(0x5000, 0x20), (0x5100, 0x10)], fde_encoding=0x0B),
                         [(0x5000, 0x20), (0x5100, 0x10)])

    def test_unsupported_header_version(self):
        _, read = build([(0x5000, 0x20)], version=2)
        with self.assertRaises(eh_frame.EhFrameError):
            eh_frame.function_ranges(read, HEADER)

    def test_unsupported_pointer_application(self):
        _, read = build([(0x5000, 0x20)], fde_encoding=0x4B)   # textrel is not something we read
        with self.assertRaises(eh_frame.EhFrameError):
            eh_frame.function_ranges(read, HEADER)


class ModuleRangesTests(unittest.TestCase):
    def module(self, functions):
        """A flat "file" where vaddr == offset, and the program headers that describe it."""
        memory, _ = build(functions)
        elf = bytearray(0x4000)
        for origin, data in memory.items():
            elf[origin:origin + len(data)] = data
        headers = [
            {'type': eh_frame.PT_LOAD, 'vaddr': 0, 'offset': 0, 'filesz': 0x4000, 'memsz': 0x4000, 'flags': 5},
            {'type': eh_frame.PT_GNU_EH_FRAME, 'vaddr': HEADER, 'offset': HEADER, 'filesz': 0x100,
             'memsz': 0x100, 'flags': 4},
        ]
        return elf, headers

    def test_adds_the_module_base(self):
        elf, headers = self.module([(0x5000, 0x20), (0x5100, 0x10)])
        record = eh_frame.module_ranges('lib.prx', elf, headers, 0x100000)
        self.assertIsNone(record['error'])
        self.assertEqual(record['ranges'], [[0x105000, 0x20], [0x105100, 0x10]])
        self.assertEqual((record['file'], record['base']), ('lib.prx', 0x100000))

    def test_missing_table_is_reported_not_raised(self):
        elf, headers = self.module([(0x5000, 0x20)])
        record = eh_frame.module_ranges('lib.prx', elf, headers[:1], 0)
        self.assertIsNone(record['ranges'])
        self.assertIn('PT_GNU_EH_FRAME', record['error'])

    def test_truncated_file_is_reported(self):
        elf, headers = self.module([(0x5000, 0x20)])
        headers[0]['filesz'] = 0x1008          # the table is cut short
        record = eh_frame.module_ranges('lib.prx', elf, headers, 0)
        self.assertIsNone(record['ranges'])
        self.assertTrue(record['error'])


if __name__ == '__main__':
    unittest.main()
