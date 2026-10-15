# SPDX-License-Identifier: GPL-2.0-or-later
"""Function ranges of an ELF module, from its .eh_frame_hdr (ADR-0017).

The ISA rewrite must patch only code: a game's .text holds data too (the linear sweep of the
SotC eboot shows ~178 thousand undecodable lines). The unwind tables list every function with
its start and size, so the generator scans those ranges and nothing else.

Reads PT_GNU_EH_FRAME (0x6474e550): the binary search table of (start, FDE) pairs, then each
FDE's start/range using the encoding its CIE declares ('R' augmentation). Only what compilers
emit for x86-64 is supported; anything else raises EhFrameError and the caller falls back to a
linear scan, with a flag in the table.

Usage as a library: function_ranges(read, header_address) with read(address, size) -> bytes.
"""
import struct

PT_LOAD = 1
PT_SCE_LOAD = 0x61000010
PT_GNU_EH_FRAME = 0x6474E550


class EhFrameError(ValueError):
    """The unwind tables are absent or use an encoding this reader does not support."""


# DW_EH_PE formats: low nibble -> (size, struct format)
_FORMATS = {0x00: (8, '<Q'), 0x02: (2, '<H'), 0x03: (4, '<I'), 0x04: (8, '<Q'),
            0x0A: (2, '<h'), 0x0B: (4, '<i'), 0x0C: (8, '<q')}
_PCREL, _DATAREL = 0x10, 0x30
_OMIT = 0xFF


def _read_encoded(read, address, encoding, header):
    """(value, size) of a DW_EH_PE-encoded pointer at address; datarel is relative to header."""
    if encoding == _OMIT:
        return None, 0
    size, fmt = _FORMATS.get(encoding & 0x0F, (0, None))
    if not fmt:
        raise EhFrameError(f'unsupported pointer format {encoding:#x}')
    value, = struct.unpack(fmt, read(address, size))
    application = encoding & 0x70
    if application == _PCREL:
        value += address
    elif application == _DATAREL:
        value += header
    elif application != 0:
        raise EhFrameError(f'unsupported pointer application {encoding:#x}')
    return value & 0xFFFFFFFFFFFFFFFF, size


def _uleb(data, pos):
    value = shift = 0
    while True:
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, pos
        shift += 7


def _sleb(data, pos):
    value = shift = 0
    while True:
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            if byte & 0x40:
                value -= 1 << shift
            return value, pos


def _cie_fde_encoding(read, cie):
    """Pointer encoding the CIE declares for its FDEs ('R'); absolute 8-byte when it has none."""
    length, = struct.unpack('<I', read(cie, 4))
    if length in (0, 0xFFFFFFFF):
        raise EhFrameError('unsupported CIE length')
    data = read(cie + 4, min(length, 128))
    if struct.unpack_from('<I', data, 0)[0] != 0:
        raise EhFrameError('CIE id is not 0')
    version = data[4]
    end = data.index(0, 5)
    augmentation = data[5:end].decode('ascii', 'replace')
    pos = end + 1
    if augmentation.startswith('eh'):
        pos += 8
    _, pos = _uleb(data, pos)   # code alignment
    _, pos = _sleb(data, pos)   # data alignment
    if version == 1:
        pos += 1                # return address register
    else:
        _, pos = _uleb(data, pos)
    encoding = 0x00
    if augmentation.startswith('z'):
        _, pos = _uleb(data, pos)   # augmentation data length
        for letter in augmentation[1:]:
            if letter == 'R':
                encoding = data[pos]
                pos += 1
            elif letter == 'L':
                pos += 1
            elif letter == 'P':
                personality = data[pos]
                pos += 1
                size, fmt = _FORMATS.get(personality & 0x0F, (0, None))
                if personality != _OMIT and not fmt:
                    raise EhFrameError('unsupported personality encoding')
                pos += size
            elif letter not in 'SB':
                raise EhFrameError(f'unsupported augmentation {letter!r}')
    return encoding


def function_ranges(read, header):
    """Sorted, merged [(start, size)] of the functions listed by the .eh_frame_hdr at header."""
    version, pointer_encoding, count_encoding, table_encoding = read(header, 4)
    if version != 1:
        raise EhFrameError(f'.eh_frame_hdr version {version}')
    if count_encoding == _OMIT or table_encoding != 0x3B:  # datarel | sdata4: what compilers emit
        raise EhFrameError('no binary search table')
    position = header + 4
    _, size = _read_encoded(read, position, pointer_encoding, header)
    position += size
    count, size = _read_encoded(read, position, count_encoding, header)
    position += size
    encodings = {}
    ranges = []
    for i in range(count):
        _, fde_offset = struct.unpack('<ii', read(position + 8 * i, 8))
        fde = header + fde_offset
        length, cie_back = struct.unpack('<II', read(fde, 8))
        if length in (0, 0xFFFFFFFF):
            continue
        cie = fde + 4 - cie_back
        if cie not in encodings:
            encodings[cie] = _cie_fde_encoding(read, cie)
        encoding = encodings[cie]
        start, size = _read_encoded(read, fde + 8, encoding, header)
        span, _ = _read_encoded(read, fde + 8 + size, encoding & 0x0F, header)
        if span:
            ranges.append((start, span))
    return merge(ranges)


def merge(ranges):
    """Sorted ranges with overlapping and touching ones joined."""
    merged = []
    for start, size in sorted(ranges):
        if merged and start <= merged[-1][0] + merged[-1][1]:
            end = max(merged[-1][0] + merged[-1][1], start + size)
            merged[-1] = (merged[-1][0], end - merged[-1][0])
        else:
            merged.append((start, size))
    return merged


def module_ranges(name, elf, program_headers, base):
    """Record of one module for code-ranges.json: absolute ranges (base added), or the error."""
    loads = [p for p in program_headers if p['type'] in (PT_LOAD, PT_SCE_LOAD)]

    def read(address, size):
        for p in loads:
            if p['vaddr'] <= address and address + size <= p['vaddr'] + p['filesz']:
                at = p['offset'] + address - p['vaddr']
                return bytes(elf[at:at + size])
        raise EhFrameError(f'address {address:#x} outside the loaded file data')

    record = {'file': name, 'base': base, 'ranges': None, 'error': None}
    header = next((p for p in program_headers if p['type'] == PT_GNU_EH_FRAME), None)
    try:
        if header is None:
            raise EhFrameError('no PT_GNU_EH_FRAME')
        record['ranges'] = [[base + start, size] for start, size in function_ranges(read, header['vaddr'])]
    except (EhFrameError, struct.error, IndexError, ValueError) as error:
        record['error'] = str(error)
    return record
