#!/usr/bin/env python3
# Part of pyobjtools — https://github.com/Maxwell175/pyobjtools
# Copyright (c) 2026 Maxwell Dreytser
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""
Cross-platform symbol lister (pure Python nm replacement).

Supports:
- ELF (.o, .so) — 32/64-bit, any endianness
- Mach-O (.o, .dylib) — 32/64-bit, any endianness
- PE/COFF (.obj) — Windows
- Clang LTO bitcode (BC\xc0\xde)
- GCC LTO slim objects (ELF with .gnu.lto_*)
- MSVC LTCG objects (ANON_OBJECT / CIL)
- Unix ar archives (.a, .lib) — symbols from all object members

Output format matches nm: <address> <type> <name>
"""
import os
import struct
import sys
import tempfile


class Symbol:
    __slots__ = ('name', 'value', 'type_char')

    def __init__(self, name, value, type_char):
        self.name = name
        self.value = value
        self.type_char = type_char

    def format(self, addr_width=16):
        if self.type_char == 'U' or self.value is None:
            addr = ' ' * addr_width
        else:
            addr = f'{self.value:0{addr_width}x}'
        return f'{addr} {self.type_char} {self.name}'


# =============================================================================
# Format detection
# =============================================================================

def detect_format(path):
    with open(path, 'rb') as f:
        header = f.read(20)

    if len(header) < 4:
        return None

    if header[:8] == b'!<arch>\n':
        return 'ar'

    if header[:4] == b'\xde\xc0\x17\x0b':
        return 'clang-lto'

    if header[:2] == b'BC' and header[2:4] == b'\xc0\xde':
        return 'clang-lto'

    if header[:4] == b'\x7fELF':
        with open(path, 'rb') as f:
            data = f.read()
        try:
            e_shoff = struct.unpack_from("<Q", data, 40)[0]
            e_shstrndx = struct.unpack_from("<H", data, 62)[0]
            shstr_off = e_shoff + e_shstrndx * 64 + 24
            shstr_start = struct.unpack_from("<Q", data, shstr_off)[0]
            shstr_size = struct.unpack_from("<Q", data, shstr_off + 8)[0]
            shstrtab = data[shstr_start:shstr_start + shstr_size]
            if b'.gnu.lto_' in shstrtab:
                return 'gcc-lto'
        except Exception:
            pass
        return 'elf'

    if header[:2] == b'\x00\x00' and header[2:4] == b'\xff\xff':
        return 'msvc-lto'

    magic32 = struct.unpack_from("<I", header, 0)[0]
    if magic32 in (0xFEEDFACF, 0xFEEDFACE, 0xCFFAEDFE, 0xCEFAEDFE):
        return 'macho'

    machine = struct.unpack_from("<H", header, 0)[0]
    if machine in (0x14c, 0x8664, 0xaa64, 0x1c0, 0x1c4):
        return 'coff'

    return None


# =============================================================================
# ELF
# =============================================================================

# ELF symbol binding
_STB_LOCAL = 0
_STB_GLOBAL = 1
_STB_WEAK = 2

# ELF symbol types
_STT_NOTYPE = 0
_STT_OBJECT = 1
_STT_FUNC = 2
_STT_SECTION = 3
_STT_FILE = 4

# ELF section types
_SHT_NOBITS = 8

# Special section indices
_SHN_UNDEF = 0
_SHN_ABS = 0xfff1
_SHN_COMMON = 0xfff2


def _elf_section_type_letter(sh_type, sh_flags):
    SHF_WRITE = 0x1
    SHF_ALLOC = 0x2
    SHF_EXECINSTR = 0x4

    if not (sh_flags & SHF_ALLOC):
        return 'n'
    if sh_flags & SHF_EXECINSTR:
        return 't'
    if sh_type == _SHT_NOBITS:
        return 'b'
    if sh_flags & SHF_WRITE:
        return 'd'
    return 'r'


def nm_elf(path):
    with open(path, 'rb') as f:
        data = f.read()

    ei_class = data[4]
    endian = "<" if data[5] == 1 else ">"

    if ei_class == 2:
        e_shoff = struct.unpack_from(endian + "Q", data, 40)[0]
        e_shentsize = struct.unpack_from(endian + "H", data, 58)[0]
        e_shnum = struct.unpack_from(endian + "H", data, 60)[0]
        sym_entry_size = 24
        off_field = "Q"
        off_offset = 24
        size_offset = 32
    else:
        e_shoff = struct.unpack_from(endian + "I", data, 32)[0]
        e_shentsize = struct.unpack_from(endian + "H", data, 46)[0]
        e_shnum = struct.unpack_from(endian + "H", data, 48)[0]
        sym_entry_size = 16
        off_field = "I"
        off_offset = 16
        size_offset = 20

    def read_shdr(idx, field_off, fmt):
        return struct.unpack_from(endian + fmt, data, e_shoff + idx * e_shentsize + field_off)[0]

    sections_type = []
    sections_flags = []
    for i in range(e_shnum):
        sections_type.append(read_shdr(i, 4, "I"))
        sections_flags.append(read_shdr(i, 8, off_field))

    symtab_idx = None
    for i in range(e_shnum):
        if sections_type[i] == 2:  # SHT_SYMTAB
            symtab_idx = i
            break

    if symtab_idx is None:
        for i in range(e_shnum):
            if sections_type[i] == 11:  # SHT_DYNSYM
                symtab_idx = i
                break

    if symtab_idx is None:
        return []

    strtab_idx = read_shdr(symtab_idx, 40 if ei_class == 2 else 24, "I")
    strtab_off = read_shdr(strtab_idx, off_offset, off_field)
    strtab_size = read_shdr(strtab_idx, size_offset, off_field)
    strtab = data[strtab_off:strtab_off + strtab_size]

    symtab_off = read_shdr(symtab_idx, off_offset, off_field)
    symtab_size = read_shdr(symtab_idx, size_offset, off_field)
    num_syms = symtab_size // sym_entry_size

    symbols = []
    for i in range(num_syms):
        entry = symtab_off + i * sym_entry_size

        if ei_class == 2:
            st_name = struct.unpack_from(endian + "I", data, entry)[0]
            st_info = data[entry + 4]
            st_shndx = struct.unpack_from(endian + "H", data, entry + 6)[0]
            st_value = struct.unpack_from(endian + "Q", data, entry + 8)[0]
        else:
            st_name = struct.unpack_from(endian + "I", data, entry)[0]
            st_value = struct.unpack_from(endian + "I", data, entry + 4)[0]
            st_info = data[entry + 12]
            st_shndx = struct.unpack_from(endian + "H", data, entry + 14)[0]

        if st_name == 0:
            continue

        end = strtab.index(b'\x00', st_name)
        name = strtab[st_name:end].decode("ascii", errors="replace")

        bind = st_info >> 4
        stype = st_info & 0xf

        if stype == _STT_FILE or stype == _STT_SECTION:
            continue

        if st_shndx == _SHN_UNDEF:
            tc = 'U' if bind != _STB_WEAK else 'w'
        elif st_shndx == _SHN_ABS:
            tc = 'A' if bind == _STB_GLOBAL else 'a'
        elif st_shndx == _SHN_COMMON:
            tc = 'C'
        elif bind == _STB_WEAK:
            tc = 'W' if stype == _STT_FUNC else 'V'
        else:
            sec_letter = _elf_section_type_letter(
                sections_type[st_shndx] if st_shndx < e_shnum else 0,
                sections_flags[st_shndx] if st_shndx < e_shnum else 0)
            tc = sec_letter.upper() if bind == _STB_GLOBAL else sec_letter

        value = st_value if st_shndx != _SHN_UNDEF else None
        symbols.append(Symbol(name, value, tc))

    return symbols


# =============================================================================
# Mach-O
# =============================================================================

_N_UNDF = 0x0
_N_ABS = 0x2
_N_SECT = 0xe
_N_INDR = 0xa

_N_EXT = 0x01
_N_PEXT = 0x10


def nm_macho(path):
    with open(path, 'rb') as f:
        data = f.read()

    magic = struct.unpack_from("<I", data, 0)[0]
    if magic == 0xFEEDFACF:
        is_64, endian = True, "<"
    elif magic == 0xFEEDFACE:
        is_64, endian = False, "<"
    elif magic == 0xCFFAEDFE:
        is_64, endian = True, ">"
    elif magic == 0xCEFAEDFE:
        is_64, endian = False, ">"
    else:
        return []

    header_size = 32 if is_64 else 28
    ncmds = struct.unpack_from(endian + "I", data, 16)[0]
    nlist_size = 16 if is_64 else 12

    symtab_cmd = None
    seg_sections = []
    offset = header_size
    for _ in range(ncmds):
        cmd = struct.unpack_from(endian + "I", data, offset)[0]
        cmdsize = struct.unpack_from(endian + "I", data, offset + 4)[0]
        if cmd == 2:  # LC_SYMTAB
            symtab_cmd = {
                "symoff": struct.unpack_from(endian + "I", data, offset + 8)[0],
                "nsyms": struct.unpack_from(endian + "I", data, offset + 12)[0],
                "stroff": struct.unpack_from(endian + "I", data, offset + 16)[0],
                "strsize": struct.unpack_from(endian + "I", data, offset + 20)[0],
            }
        elif cmd in (25, 1):  # LC_SEGMENT_64, LC_SEGMENT
            if cmd == 25:
                nsects = struct.unpack_from(endian + "I", data, offset + 64)[0]
                sec_start = offset + 72
                sec_size = 80
            else:
                nsects = struct.unpack_from(endian + "I", data, offset + 48)[0]
                sec_start = offset + 56
                sec_size = 68
            for s in range(nsects):
                sec_off = sec_start + s * sec_size
                sectname = data[sec_off:sec_off + 16].rstrip(b'\x00').decode('ascii', errors='replace')
                segname = data[sec_off + 16:sec_off + 32].rstrip(b'\x00').decode('ascii', errors='replace')
                seg_sections.append((segname, sectname))
        offset += cmdsize

    if symtab_cmd is None:
        return []

    stroff = symtab_cmd["stroff"]
    strsize = symtab_cmd["strsize"]
    strtab = data[stroff:stroff + strsize]
    symoff = symtab_cmd["symoff"]
    nsyms = symtab_cmd["nsyms"]

    symbols = []
    for i in range(nsyms):
        entry = symoff + i * nlist_size
        n_un = struct.unpack_from(endian + "I", data, entry)[0]
        n_type = data[entry + 4]
        n_sect = data[entry + 5]
        if is_64:
            n_value = struct.unpack_from(endian + "Q", data, entry + 8)[0]
        else:
            n_value = struct.unpack_from(endian + "I", data, entry + 8)[0]

        if n_un == 0 or n_un >= len(strtab):
            continue

        end = strtab.index(b'\x00', n_un)
        name = strtab[n_un:end].decode("ascii", errors="replace")

        if n_type & 0x20:  # N_STAB
            continue

        type_field = n_type & 0x0e
        is_ext = bool(n_type & _N_EXT)

        if type_field == _N_UNDF:
            if n_value != 0:
                tc = 'C' if is_ext else 'c'
            else:
                tc = 'U'
        elif type_field == _N_ABS:
            tc = 'A' if is_ext else 'a'
        elif type_field == _N_SECT:
            sec_idx = n_sect - 1
            if sec_idx < len(seg_sections):
                seg, sect = seg_sections[sec_idx]
                if seg == '__TEXT' and sect == '__text':
                    tc = 't'
                elif seg == '__DATA' and sect == '__bss':
                    tc = 'b'
                elif seg == '__DATA':
                    tc = 'd'
                elif seg == '__TEXT':
                    tc = 'r'
                else:
                    tc = 's'
            else:
                tc = 's'
            if is_ext:
                tc = tc.upper()
        elif type_field == _N_INDR:
            tc = 'I' if is_ext else 'i'
        else:
            tc = '?'

        value = n_value if type_field != _N_UNDF or n_value != 0 else None
        symbols.append(Symbol(name, value, tc))

    return symbols


# =============================================================================
# PE/COFF
# =============================================================================

# COFF symbol storage classes
_IMAGE_SYM_CLASS_EXTERNAL = 2
_IMAGE_SYM_CLASS_STATIC = 3
_IMAGE_SYM_CLASS_LABEL = 6
_IMAGE_SYM_CLASS_FILE = 103
_IMAGE_SYM_CLASS_SECTION = 104

_IMAGE_SYM_UNDEFINED = 0


def nm_coff(path):
    with open(path, 'rb') as f:
        data = f.read()

    num_sections = struct.unpack_from("<H", data, 2)[0]
    sym_table_off = struct.unpack_from("<I", data, 8)[0]
    num_syms = struct.unpack_from("<I", data, 12)[0]

    if sym_table_off == 0 or num_syms == 0:
        return []

    strtab_off = sym_table_off + num_syms * 18
    if strtab_off < len(data):
        strtab_size = struct.unpack_from("<I", data, strtab_off)[0]
        strtab = data[strtab_off:strtab_off + strtab_size]
    else:
        strtab_size = 4
        strtab = b'\x00\x00\x00\x00'

    sec_headers_off = 20 + struct.unpack_from("<H", data, 16)[0]
    section_chars = []
    for i in range(num_sections):
        sec_off = sec_headers_off + i * 40
        chars = struct.unpack_from("<I", data, sec_off + 36)[0]
        section_chars.append(chars)

    symbols = []
    i = 0
    while i < num_syms:
        entry_off = sym_table_off + i * 18
        name_field = data[entry_off:entry_off + 8]
        zeroes = struct.unpack_from("<I", name_field, 0)[0]
        num_aux = data[entry_off + 17]

        if zeroes == 0:
            str_offset = struct.unpack_from("<I", name_field, 4)[0]
            if str_offset < strtab_size:
                end = strtab.index(b'\x00', str_offset)
                name = strtab[str_offset:end].decode("ascii", errors="replace")
            else:
                name = ""
        else:
            end = name_field.find(b'\x00')
            if end == -1:
                end = 8
            name = name_field[:end].decode("ascii", errors="replace")

        value = struct.unpack_from("<I", data, entry_off + 8)[0]
        sect_num = struct.unpack_from("<h", data, entry_off + 12)[0]
        storage_class = data[entry_off + 16]

        i += 1 + num_aux

        if storage_class == _IMAGE_SYM_CLASS_FILE:
            continue
        if storage_class == _IMAGE_SYM_CLASS_SECTION:
            continue
        if not name or name.startswith('.'):
            continue

        is_external = (storage_class == _IMAGE_SYM_CLASS_EXTERNAL)

        if sect_num == _IMAGE_SYM_UNDEFINED:
            if value == 0:
                tc = 'U'
            else:
                tc = 'C' if is_external else 'c'
        elif sect_num > 0 and sect_num <= num_sections:
            chars = section_chars[sect_num - 1]
            IMAGE_SCN_CNT_CODE = 0x20
            IMAGE_SCN_CNT_UNINIT = 0x80
            IMAGE_SCN_MEM_WRITE = 0x80000000
            IMAGE_SCN_MEM_READ = 0x40000000

            if chars & IMAGE_SCN_CNT_CODE:
                tc = 't'
            elif chars & IMAGE_SCN_CNT_UNINIT:
                tc = 'b'
            elif chars & IMAGE_SCN_MEM_WRITE:
                tc = 'd'
            elif chars & IMAGE_SCN_MEM_READ:
                tc = 'r'
            else:
                tc = 'n'
            if is_external:
                tc = tc.upper()
        elif sect_num == -1:
            tc = 'A' if is_external else 'a'
        elif sect_num == -2:
            tc = 'D' if is_external else 'd'
        else:
            tc = '?'

        symbols.append(Symbol(name, value if sect_num != _IMAGE_SYM_UNDEFINED else None, tc))

    return symbols


# =============================================================================
# Clang LTO (LLVM bitcode)
# =============================================================================

def _unwrap_bitcode(data):
    if data[:4] == b'\xde\xc0\x17\x0b':
        offset = struct.unpack_from('<I', data, 8)[0]
        size = struct.unpack_from('<I', data, 12)[0]
        return data[offset:offset + size]
    return data


def nm_clang_lto(path):
    with open(path, 'rb') as f:
        data = _unwrap_bitcode(f.read())

    blocks = []
    pos_bits = 32

    def read_bits(n):
        nonlocal pos_bits
        result = 0
        bits_read = 0
        while bits_read < n:
            byte_idx = pos_bits // 8
            bit_idx = pos_bits % 8
            avail = 8 - bit_idx
            take = min(avail, n - bits_read)
            bits = (data[byte_idx] >> bit_idx) & ((1 << take) - 1)
            result |= bits << bits_read
            bits_read += take
            pos_bits += take
        return result

    def read_vbr(width):
        result = 0
        shift = 0
        while True:
            chunk = read_bits(width)
            result |= (chunk & ((1 << (width - 1)) - 1)) << shift
            shift += width - 1
            if (chunk & (1 << (width - 1))) == 0:
                break
        return result

    def align32():
        nonlocal pos_bits
        r = pos_bits % 32
        if r:
            pos_bits += 32 - r

    while pos_bits // 8 < len(data):
        abbrevid = read_bits(2)
        if abbrevid != 1:
            break
        block_id = read_vbr(8)
        read_vbr(4)
        align32()
        byte_pos = pos_bits // 8
        block_len = struct.unpack_from("<I", data, byte_pos)[0]
        pos_bits += 32
        content_start = pos_bits // 8
        content_end = content_start + block_len * 4
        blocks.append((block_id, content_start, content_end))
        pos_bits = content_end * 8

    strtab_block = None
    symtab_block = None
    for block_id, cs, ce in blocks:
        if block_id == 23:
            strtab_block = data[cs:ce]
        elif block_id == 25:
            symtab_block = data[cs:ce]

    if strtab_block is None:
        return []

    symbols = []

    if symtab_block and len(symtab_block) > 24:
        raw_strtab = bytes(strtab_block)
        raw_symtab = bytes(symtab_block)

        best_result = None
        best_count = 0

        for str_skip in range(0, min(len(raw_strtab), 64)):
            strtab_data = raw_strtab[str_skip:]
            strtab_len = len(strtab_data)
            if strtab_len < 1:
                continue
            b0 = strtab_data[0]
            if not (b0 == 95 or 48 <= b0 <= 57 or 65 <= b0 <= 90 or 97 <= b0 <= 122):
                continue

            for sym_skip in range(0, min(len(raw_symtab) - 24, 128), 4):
                symtab_blob = raw_symtab[sym_skip:]
                blob_size = len(symtab_blob)
                count = 0
                pos = 0
                while pos + 24 <= blob_size:
                    name_off = struct.unpack_from("<I", symtab_blob, pos)[0]
                    name_sz = struct.unpack_from("<I", symtab_blob, pos + 4)[0]
                    if (0 < name_sz < 4096
                            and name_off + name_sz <= strtab_len
                            and all(b == 95 or 48 <= b <= 57 or 65 <= b <= 90 or 97 <= b <= 122
                                    for b in strtab_data[name_off:name_off + name_sz])):
                        count += 1
                        pos += 24
                    else:
                        break
                if count > best_count:
                    best_count = count
                    best_result = (str_skip, sym_skip, count)

        if best_result and best_count >= 3:
            str_skip, sym_skip, count = best_result
            strtab_data = raw_strtab[str_skip:]
            symtab_blob = raw_symtab[sym_skip:]
            for i in range(count):
                pos = i * 24
                name_off = struct.unpack_from("<I", symtab_blob, pos)[0]
                name_sz = struct.unpack_from("<I", symtab_blob, pos + 4)[0]
                flags = struct.unpack_from("<I", symtab_blob, pos + 20)[0]

                name = strtab_data[name_off:name_off + name_sz].decode("ascii", errors="replace")

                if flags & 0x1:
                    tc = 'U'
                elif flags & 0x4:
                    tc = 'W'
                elif flags & 0x2:
                    tc = 'C'
                else:
                    tc = 'T'
                symbols.append(Symbol(name, None, tc))

    return symbols


# =============================================================================
# GCC LTO
# =============================================================================

def nm_gcc_lto(path):
    with open(path, 'rb') as f:
        data = f.read()

    e_shoff = struct.unpack_from("<Q", data, 40)[0]
    e_shnum = struct.unpack_from("<H", data, 60)[0]
    e_shstrndx = struct.unpack_from("<H", data, 62)[0]

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * 64
        sections.append({
            "sh_name": struct.unpack_from("<I", data, off)[0],
            "sh_type": struct.unpack_from("<I", data, off + 4)[0],
            "sh_offset": struct.unpack_from("<Q", data, off + 24)[0],
            "sh_size": struct.unpack_from("<Q", data, off + 32)[0],
            "sh_link": struct.unpack_from("<I", data, off + 40)[0],
        })

    shstr = sections[e_shstrndx]
    shstrtab = data[shstr["sh_offset"]:shstr["sh_offset"] + shstr["sh_size"]]

    def sec_name(idx):
        off = sections[idx]["sh_name"]
        end = shstrtab.index(b"\x00", off)
        return shstrtab[off:end].decode("ascii")

    def _try_decompress(raw):
        if raw[:4] == b'\x28\xb5\x2f\xfd':
            try:
                try:
                    from compression import zstd
                    return zstd.decompress(raw)
                except ImportError:
                    pass
                import zstandard
                return zstandard.ZstdDecompressor().decompress(raw)
            except Exception:
                return raw
        return raw

    # Each entry in .gnu.lto_.symtab.<hash> is:
    #   <name>\x00 + 15-byte fixed record
    # Record layout (matches enum gcc_plugin_symbol_kind from gcc/lto-streamer.h):
    #   byte 0: padding (0x00)
    #   byte 1: kind  — 0=DEF, 1=WEAKDEF, 2=UNDEF, 3=WEAKUNDEF, 4=COMMON
    #   bytes 2-14: visibility/size/etc. (ignored)
    # We map kind to GNU nm's type letters so callers like rebuildpython.py
    # that filter on " u " (undefined) get the same defined/undefined split
    # they got from /usr/bin/nm with the binutils LTO plugin loaded.
    _LTO_KIND_TO_TYPE = {
        0: 'T',  # DEF
        1: 'W',  # WEAKDEF
        2: 'U',  # UNDEF
        3: 'w',  # WEAKUNDEF
        4: 'C',  # COMMON
    }
    _LTO_RECORD_SIZE = 15

    symbols = []
    for i in range(e_shnum):
        name = sec_name(i)
        if name.startswith(".gnu.lto_.symtab."):
            sec = sections[i]
            sec_data = _try_decompress(data[sec["sh_offset"]:sec["sh_offset"] + sec["sh_size"]])
            pos = 0
            while pos < len(sec_data):
                end = sec_data.find(b'\x00', pos)
                if end == -1 or end == pos:
                    break
                sym_name = sec_data[pos:end].decode("ascii", errors="replace")
                rec_start = end + 1
                if rec_start + _LTO_RECORD_SIZE > len(sec_data):
                    break
                kind = sec_data[rec_start + 1]
                pos = rec_start + _LTO_RECORD_SIZE
                if not sym_name.isprintable():
                    continue
                type_char = _LTO_KIND_TO_TYPE.get(kind, 'T')
                symbols.append(Symbol(sym_name, None, type_char))

    return symbols


# =============================================================================
# MSVC LTCG
# =============================================================================

def nm_msvc_lto(path):
    with open(path, 'rb') as f:
        data = f.read()

    num_sections = struct.unpack_from('<H', data, 0x22)[0]
    symbols = []

    ptr_symtab = struct.unpack_from('<I', data, 0x28)[0]
    num_syms = struct.unpack_from('<I', data, 0x2C)[0]

    if ptr_symtab == 0 or num_syms == 0:
        return symbols

    strtab_off = ptr_symtab + num_syms * 18
    if strtab_off < len(data):
        strtab_size = struct.unpack_from("<I", data, strtab_off)[0]
        strtab = data[strtab_off:strtab_off + strtab_size]
    else:
        strtab_size = 4
        strtab = b'\x00\x00\x00\x00'

    i = 0
    while i < num_syms:
        entry_off = ptr_symtab + i * 18
        if entry_off + 18 > len(data):
            break

        name_field = data[entry_off:entry_off + 8]
        zeroes = struct.unpack_from("<I", name_field, 0)[0]
        num_aux = data[entry_off + 17]

        if zeroes == 0:
            str_offset = struct.unpack_from("<I", name_field, 4)[0]
            if str_offset < strtab_size:
                end = strtab.index(b'\x00', str_offset)
                name = strtab[str_offset:end].decode("ascii", errors="replace")
            else:
                name = ""
        else:
            end = name_field.find(b'\x00')
            if end == -1:
                end = 8
            name = name_field[:end].decode("ascii", errors="replace")

        sect_num = struct.unpack_from("<h", data, entry_off + 12)[0]
        storage_class = data[entry_off + 16]

        i += 1 + num_aux

        if storage_class == _IMAGE_SYM_CLASS_FILE:
            continue
        if not name or name.startswith('.'):
            continue

        is_external = (storage_class == _IMAGE_SYM_CLASS_EXTERNAL)

        if sect_num == 0:
            tc = 'U'
        else:
            tc = 'T' if is_external else 't'

        symbols.append(Symbol(name, None, tc))

    return symbols


# =============================================================================
# Public API
# =============================================================================

def nm_ar(path):
    """List symbols from a Unix ar archive (.a, .lib).

    Combines two sources:

    1. The archive's own symbol-directory member (the leading "/" or
       "__.SYMDEF" entry that every linker-friendly archive carries). This
       enumerates every name that SOME member defines, regardless of the
       individual member's object format. It is the only way to surface
       symbols from members in MSVC LTCG IR (ANON_OBJECT) form, which the
       per-member parser cannot decode.

    2. Per-member nm output for the formats we understand. Adds undefined
       symbols (which the archive directory does not list) that callers
       like rebuildpython.py's dep-order algorithm need.

    Defined symbols seen by both sources are deduplicated.
    """
    from . import ar as _ar

    # Step 1: archive's defined-symbol directory.
    seen_defined = set()
    symbols = []
    for name in _ar.read_symbol_directory(path):
        if not name or name in seen_defined:
            continue
        seen_defined.add(name)
        symbols.append(Symbol(name, None, 'T'))

    # Step 2: per-member symbols. Skip members we can't parse — for those,
    # the directory above is the best we have.
    with tempfile.TemporaryDirectory() as tmpdir:
        for member_path in _ar.extract_archive(path, tmpdir):
            try:
                member_syms = nm(member_path)
            except ValueError:
                continue
            for s in member_syms:
                if s.type_char in ('U', 'w'):
                    symbols.append(s)
                elif s.name not in seen_defined:
                    seen_defined.add(s.name)
                    symbols.append(s)
    return symbols


def nm(path):
    """List symbols from an object file or ar archive. Returns list of Symbol objects."""
    fmt = detect_format(path)
    if fmt == 'elf' or fmt == 'gcc-lto':
        if fmt == 'gcc-lto':
            return nm_gcc_lto(path)
        return nm_elf(path)
    elif fmt == 'macho':
        return nm_macho(path)
    elif fmt == 'coff':
        return nm_coff(path)
    elif fmt == 'clang-lto':
        return nm_clang_lto(path)
    elif fmt == 'msvc-lto':
        return nm_msvc_lto(path)
    elif fmt == 'ar':
        return nm_ar(path)
    else:
        raise ValueError(f"{path}: unrecognized object file format")


# =============================================================================
# CLI
# =============================================================================

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: nm.py <obj_file> [obj_file ...]", file=sys.stderr)
        sys.exit(1)

    multi = len(sys.argv) > 2
    for path in sys.argv[1:]:
        if multi:
            print(f"\n{path}:")
        try:
            fmt = detect_format(path)
            addr_width = 8 if fmt == 'coff' else 16
            for sym in nm(path):
                print(sym.format(addr_width))
        except (ValueError, OSError) as e:
            print(f"nm: {e}", file=sys.stderr)
