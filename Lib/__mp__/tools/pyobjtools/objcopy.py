#!/usr/bin/env python3
# Part of pyobjtools — https://github.com/Maxwell175/pyobjtools
# Copyright (c) 2026 Maxwell Dreytser
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""
Cross-platform object file symbol renamer (pure Python objcopy --redefine-syms).

Supports all common object formats:
- ELF (.o) — Linux/Unix, both 32-bit and 64-bit, any endianness
- Mach-O (.o) — macOS/iOS, both 32-bit and 64-bit
- PE/COFF (.obj) — Windows (MSVC, clang-cl)
- GCC LTO slim objects (ELF with .gnu.lto_* sections)
- Clang LTO bitcode (raw LLVM bitcode, magic BC\\xc0\\xde)
- MSVC LTCG objects (ANON_OBJECT / CIL format from /GL)

For LTO objects, preserves the LTO format without materialization.
For regular objects, rebuilds string tables with new names.

Use rename_obj() for auto-detection, or call format-specific functions directly.
"""
import struct
import subprocess
import os
import re
import tempfile

def _zstd_decompress(compressed):
    try:
        from compression import zstd
        return zstd.decompress(compressed)
    except ImportError:
        pass
    import zstandard
    return zstandard.ZstdDecompressor().decompress(compressed)


def _zstd_compress(data, level=3):
    try:
        from compression import zstd
        return zstd.compress(data, level=level)
    except ImportError:
        pass
    import zstandard
    return zstandard.ZstdCompressor(level=level).compress(data)


# =============================================================================
# Shared utilities
# =============================================================================

def _encode_uleb128(val):
    result = []
    while True:
        byte = val & 0x7f
        val >>= 7
        if val:
            byte |= 0x80
        result.append(byte)
        if not val:
            break
    return bytes(result)


def _all_same_length(rename_map):
    return all(len(old) == len(new) for old, new in rename_map.items())


# =============================================================================
# Format detection
# =============================================================================

def detect_obj_format(path):
    """Detect the object file format.

    Returns: 'elf', 'macho', 'coff', 'msvc-lto', 'clang-lto', 'gcc-lto', or None.
    """
    with open(path, 'rb') as f:
        header = f.read(20)

    if len(header) < 4:
        return None

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
# ELF (regular object files)
# =============================================================================

def rename_elf(obj_path, rename_map, output_path=None):
    """Rename symbols in a regular ELF object file (.o).

    Rebuilds .strtab with new names and updates st_name offsets in .symtab.
    Handles the case where .strtab is also the section header string table.
    Works for both ELF32 and ELF64, any endianness.
    """
    if output_path is None:
        output_path = obj_path

    with open(obj_path, "rb") as f:
        data = bytearray(f.read())

    if data[:4] != b'\x7fELF':
        raise ValueError(f"{obj_path} is not an ELF file")

    ei_class = data[4]
    endian = "<" if data[5] == 1 else ">"

    if ei_class == 2:
        e_shoff = struct.unpack_from(endian + "Q", data, 40)[0]
        e_shentsize = struct.unpack_from(endian + "H", data, 58)[0]
        e_shnum = struct.unpack_from(endian + "H", data, 60)[0]
        e_shstrndx = struct.unpack_from(endian + "H", data, 62)[0]
        sym_entry_size = 24
        off_field = "Q"
        off_offset = 24
        size_offset = 32
        ehdr_shoff_offset = 40
    else:
        e_shoff = struct.unpack_from(endian + "I", data, 32)[0]
        e_shentsize = struct.unpack_from(endian + "H", data, 46)[0]
        e_shnum = struct.unpack_from(endian + "H", data, 48)[0]
        e_shstrndx = struct.unpack_from(endian + "H", data, 50)[0]
        sym_entry_size = 16
        off_field = "I"
        off_offset = 16
        size_offset = 20
        ehdr_shoff_offset = 32

    def read_shdr_field(idx, field_off, fmt):
        return struct.unpack_from(endian + fmt, data, e_shoff + idx * e_shentsize + field_off)[0]

    symtab_idx = None
    for i in range(e_shnum):
        if read_shdr_field(i, 4, "I") == 2:  # SHT_SYMTAB
            symtab_idx = i
            break

    if symtab_idx is None:
        with open(output_path, "wb") as f:
            f.write(data)
        return 0

    strtab_idx = read_shdr_field(symtab_idx, 40 if ei_class == 2 else 24, "I")
    strtab_file_off = read_shdr_field(strtab_idx, off_offset, off_field)
    strtab_size = read_shdr_field(strtab_idx, size_offset, off_field)
    strtab = bytes(data[strtab_file_off:strtab_file_off + strtab_size])

    symtab_off = read_shdr_field(symtab_idx, off_offset, off_field)
    symtab_size = read_shdr_field(symtab_idx, size_offset, off_field)
    num_syms = symtab_size // sym_entry_size

    def strtab_string(offset):
        if offset == 0:
            return ""
        end = strtab.index(b'\x00', offset)
        return strtab[offset:end].decode("ascii", errors="replace")

    def add_string(new_strtab, s):
        s_bytes = s.encode("ascii") + b'\x00'
        pos = new_strtab.find(b'\x00' + s_bytes)
        if pos != -1:
            return pos + 1
        if new_strtab[:len(s_bytes)] == s_bytes:
            return 0
        pos = len(new_strtab)
        new_strtab += s_bytes
        return pos

    new_strtab = bytearray(b'\x00')
    is_shared_strtab = (strtab_idx == e_shstrndx)

    sh_name_map = {}
    if is_shared_strtab:
        for i in range(e_shnum):
            sh_name_off = read_shdr_field(i, 0, "I")
            sec_name = strtab_string(sh_name_off)
            sh_name_map[i] = add_string(new_strtab, sec_name)

    sym_name_map = {}
    renamed = 0
    for i in range(num_syms):
        entry_off = symtab_off + i * sym_entry_size
        st_name = struct.unpack_from(endian + "I", data, entry_off)[0]
        if st_name == 0:
            sym_name_map[i] = 0
            continue
        sym_name = strtab_string(st_name)
        if sym_name in rename_map:
            new_name = rename_map[sym_name]
            renamed += 1
        else:
            new_name = sym_name
        sym_name_map[i] = add_string(new_strtab, new_name)

    for i in range(num_syms):
        entry_off = symtab_off + i * sym_entry_size
        struct.pack_into(endian + "I", data, entry_off, sym_name_map[i])

    size_diff = len(new_strtab) - strtab_size
    new_data = bytearray(data[:strtab_file_off])
    new_data += new_strtab
    new_data += data[strtab_file_off + strtab_size:]

    if size_diff != 0:
        strtab_hdr_off = e_shoff + strtab_idx * e_shentsize
        if strtab_hdr_off > strtab_file_off:
            strtab_hdr_off += size_diff
        struct.pack_into(endian + off_field, new_data, strtab_hdr_off + size_offset, len(new_strtab))

        new_e_shoff = e_shoff
        if e_shoff > strtab_file_off:
            new_e_shoff += size_diff
            struct.pack_into(endian + off_field, new_data, ehdr_shoff_offset, new_e_shoff)

        for i in range(e_shnum):
            hdr_off = new_e_shoff + i * e_shentsize
            if i == strtab_idx:
                continue
            sec_off = struct.unpack_from(endian + off_field, new_data, hdr_off + off_offset)[0]
            if sec_off > strtab_file_off:
                struct.pack_into(endian + off_field, new_data, hdr_off + off_offset, sec_off + size_diff)

    if is_shared_strtab:
        new_e_shoff = struct.unpack_from(endian + off_field, new_data, ehdr_shoff_offset)[0]
        for i in range(e_shnum):
            hdr_off = new_e_shoff + i * e_shentsize
            struct.pack_into(endian + "I", new_data, hdr_off, sh_name_map[i])

    with open(output_path, "wb") as f:
        f.write(new_data)
    return renamed


# =============================================================================
# Mach-O (regular object files)
# =============================================================================

def rename_macho(obj_path, rename_map, output_path=None):
    """Rename symbols in a Mach-O object file (.o).

    Rebuilds the string table and updates nlist entries.
    Supports both 32-bit and 64-bit Mach-O, any endianness.
    """
    if output_path is None:
        output_path = obj_path

    with open(obj_path, "rb") as f:
        data = bytearray(f.read())

    magic = struct.unpack_from("<I", data, 0)[0]
    if magic == 0xFEEDFACF:
        is_64 = True
        endian = "<"
    elif magic == 0xFEEDFACE:
        is_64 = False
        endian = "<"
    elif magic == 0xCFFAEDFE:
        is_64 = True
        endian = ">"
    elif magic == 0xCEFAEDFE:
        is_64 = False
        endian = ">"
    else:
        raise ValueError(f"{obj_path} is not a Mach-O file")

    header_size = 32 if is_64 else 28
    ncmds = struct.unpack_from(endian + "I", data, 16)[0]
    nlist_size = 16 if is_64 else 12

    symtab_cmd = None
    offset = header_size
    for _ in range(ncmds):
        cmd = struct.unpack_from(endian + "I", data, offset)[0]
        cmdsize = struct.unpack_from(endian + "I", data, offset + 4)[0]
        if cmd == 2:  # LC_SYMTAB
            symtab_cmd = {
                "cmd_offset": offset,
                "symoff": struct.unpack_from(endian + "I", data, offset + 8)[0],
                "nsyms": struct.unpack_from(endian + "I", data, offset + 12)[0],
                "stroff": struct.unpack_from(endian + "I", data, offset + 16)[0],
                "strsize": struct.unpack_from(endian + "I", data, offset + 20)[0],
            }
            break
        offset += cmdsize

    if symtab_cmd is None:
        with open(output_path, "wb") as f:
            f.write(data)
        return 0

    stroff = symtab_cmd["stroff"]
    strsize = symtab_cmd["strsize"]
    strtab = data[stroff:stroff + strsize]
    symoff = symtab_cmd["symoff"]
    nsyms = symtab_cmd["nsyms"]

    new_strtab = bytearray(b'\x00')
    offset_map = {}
    renamed = 0

    for i in range(nsyms):
        entry_off = symoff + i * nlist_size
        n_un = struct.unpack_from(endian + "I", data, entry_off)[0]

        if n_un == 0 or n_un >= len(strtab):
            offset_map[i] = 0
            continue

        end = strtab.index(b'\x00', n_un)
        sym_name = strtab[n_un:end].decode("ascii", errors="replace")

        lookup_name = sym_name[1:] if sym_name.startswith("_") else sym_name
        if lookup_name in rename_map:
            new_name = "_" + rename_map[lookup_name] if sym_name.startswith("_") else rename_map[lookup_name]
            renamed += 1
        elif sym_name in rename_map:
            new_name = rename_map[sym_name]
            renamed += 1
        else:
            new_name = sym_name

        new_name_bytes = new_name.encode("ascii") + b'\x00'
        existing_pos = new_strtab.find(new_name_bytes)
        if existing_pos != -1 and (existing_pos == 0 or new_strtab[existing_pos - 1:existing_pos] == b'\x00'):
            offset_map[i] = existing_pos
        else:
            offset_map[i] = len(new_strtab)
            new_strtab += new_name_bytes

    while len(new_strtab) % 4 != 0:
        new_strtab += b'\x00'

    for i in range(nsyms):
        entry_off = symoff + i * nlist_size
        struct.pack_into(endian + "I", data, entry_off, offset_map[i])

    size_diff = len(new_strtab) - strsize
    new_data = bytearray(data[:stroff])
    new_data += new_strtab
    new_data += data[stroff + strsize:]

    cmd_off = symtab_cmd["cmd_offset"]
    struct.pack_into(endian + "I", new_data, cmd_off + 20, len(new_strtab))

    if size_diff != 0:
        offset = header_size
        for _ in range(ncmds):
            cmd = struct.unpack_from(endian + "I", new_data, offset)[0]
            cmdsize = struct.unpack_from(endian + "I", new_data, offset + 4)[0]
            if cmd == 25:  # LC_SEGMENT_64
                seg_fileoff = struct.unpack_from(endian + "Q", new_data, offset + 40)[0]
                seg_filesize = struct.unpack_from(endian + "Q", new_data, offset + 48)[0]
                if seg_fileoff + seg_filesize > stroff:
                    struct.pack_into(endian + "Q", new_data, offset + 48, seg_filesize + size_diff)
            elif cmd == 1:  # LC_SEGMENT
                seg_fileoff = struct.unpack_from(endian + "I", new_data, offset + 28)[0]
                seg_filesize = struct.unpack_from(endian + "I", new_data, offset + 32)[0]
                if seg_fileoff + seg_filesize > stroff:
                    struct.pack_into(endian + "I", new_data, offset + 32, seg_filesize + size_diff)
            offset += cmdsize

    with open(output_path, "wb") as f:
        f.write(new_data)
    return renamed


# =============================================================================
# PE/COFF (regular object files)
# =============================================================================

def rename_coff(obj_path, rename_map, output_path=None):
    """Rename symbols in a COFF object file (.obj on Windows).

    Rebuilds the string table and updates symbol name references.
    Handles inline names (<=8 bytes) and string table references.
    """
    if output_path is None:
        output_path = obj_path

    with open(obj_path, "rb") as f:
        data = bytearray(f.read())

    machine = struct.unpack_from("<H", data, 0)[0]
    if machine not in (0x14c, 0x8664, 0xaa64, 0x1c0, 0x1c4):
        raise ValueError(f"{obj_path} is not a valid COFF file (machine=0x{machine:x})")

    num_syms = struct.unpack_from("<I", data, 12)[0]
    sym_table_off = struct.unpack_from("<I", data, 8)[0]

    if sym_table_off == 0 or num_syms == 0:
        with open(output_path, "wb") as f:
            f.write(data)
        return 0

    strtab_off = sym_table_off + num_syms * 18
    if strtab_off >= len(data):
        strtab_size = 4
        strtab = b'\x00\x00\x00\x00'
    else:
        strtab_size = struct.unpack_from("<I", data, strtab_off)[0]
        strtab = bytes(data[strtab_off:strtab_off + strtab_size])

    new_strtab = bytearray(struct.pack("<I", 0))
    renamed = 0

    i = 0
    while i < num_syms:
        entry_off = sym_table_off + i * 18
        name_field = bytes(data[entry_off:entry_off + 8])
        zeroes = struct.unpack_from("<I", name_field, 0)[0]
        num_aux = data[entry_off + 17]

        if zeroes == 0:
            str_offset = struct.unpack_from("<I", name_field, 4)[0]
            if str_offset < strtab_size:
                end = strtab.index(b'\x00', str_offset)
                sym_name = strtab[str_offset:end].decode("ascii", errors="replace")
            else:
                sym_name = ""
        else:
            end = name_field.find(b'\x00')
            if end == -1:
                end = 8
            sym_name = name_field[:end].decode("ascii", errors="replace")

        if sym_name in rename_map:
            new_name = rename_map[sym_name]
            renamed += 1
        else:
            new_name = sym_name

        new_name_bytes = new_name.encode("ascii")
        if len(new_name_bytes) <= 8:
            padded = new_name_bytes + b'\x00' * (8 - len(new_name_bytes))
            data[entry_off:entry_off + 8] = padded
        else:
            new_name_terminated = new_name_bytes + b'\x00'
            existing_pos = new_strtab.find(new_name_terminated, 4)
            if existing_pos != -1:
                new_offset = existing_pos
            else:
                new_offset = len(new_strtab)
                new_strtab += new_name_terminated
            struct.pack_into("<I", data, entry_off, 0)
            struct.pack_into("<I", data, entry_off + 4, new_offset)

        i += 1 + num_aux

    struct.pack_into("<I", new_strtab, 0, len(new_strtab))

    new_data = bytearray(data[:strtab_off])
    new_data += new_strtab

    with open(output_path, "wb") as f:
        f.write(new_data)
    return renamed


# =============================================================================
# GCC LTO (ELF with .gnu.lto_* sections)
# =============================================================================

def _gcc_lto_parse_elf_sections(data):
    """Parse section headers from a GCC LTO slim object (always ELF64 LE)."""
    e_shoff = struct.unpack_from("<Q", data, 40)[0]
    e_shnum = struct.unpack_from("<H", data, 60)[0]
    e_shstrndx = struct.unpack_from("<H", data, 62)[0]

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * 64
        sections.append({
            "sh_name": struct.unpack_from("<I", data, off)[0],
            "sh_type": struct.unpack_from("<I", data, off + 4)[0],
            "sh_flags": struct.unpack_from("<Q", data, off + 8)[0],
            "sh_addr": struct.unpack_from("<Q", data, off + 16)[0],
            "sh_offset": struct.unpack_from("<Q", data, off + 24)[0],
            "sh_size": struct.unpack_from("<Q", data, off + 32)[0],
            "sh_link": struct.unpack_from("<I", data, off + 40)[0],
            "sh_info": struct.unpack_from("<I", data, off + 44)[0],
            "sh_addralign": struct.unpack_from("<Q", data, off + 48)[0],
            "sh_entsize": struct.unpack_from("<Q", data, off + 56)[0],
        })

    shstr = sections[e_shstrndx]
    shstrtab = bytearray(data[shstr["sh_offset"]:shstr["sh_offset"] + shstr["sh_size"]])

    sec_names = []
    sec_data = []
    for sec in sections:
        off = sec["sh_name"]
        end = shstrtab.index(b"\x00", off)
        sec_names.append(shstrtab[off:end].decode("ascii"))
        if sec["sh_type"] == 0 or sec["sh_size"] == 0 or sec["sh_type"] == 8:
            sec_data.append(bytearray())
        else:
            sec_data.append(bytearray(data[sec["sh_offset"]:sec["sh_offset"] + sec["sh_size"]]))

    return sections, sec_names, sec_data, shstrtab, e_shstrndx


def _gcc_lto_rebuild_elf(data, sections, sec_names, sec_data, shstrtab, e_shstrndx, rename_map):
    """Rebuild a GCC LTO ELF file from modified section data."""
    new_sh_names = []
    for idx in range(len(sections)):
        old_name = sec_names[idx]
        new_name = old_name
        for old, new in rename_map.items():
            new_name = new_name.replace(old, new)
        target = new_name.encode() + b"\x00"
        pos = shstrtab.find(target)
        new_sh_names.append(pos if pos != -1 else sections[idx]["sh_name"])

    elf_hdr = bytearray(data[:64])
    body = bytearray()
    current_offset = 64
    new_offsets = []

    for i, sec in enumerate(sections):
        if sec["sh_type"] == 0:
            new_offsets.append(0)
            continue
        if sec["sh_type"] == 8:  # SHT_NOBITS
            new_offsets.append(current_offset)
            continue
        align = max(sec["sh_addralign"], 1)
        padding = (align - (current_offset % align)) % align
        body += b"\x00" * padding
        current_offset += padding
        new_offsets.append(current_offset)
        body += sec_data[i]
        current_offset += len(sec_data[i])

    sh_offset = current_offset
    struct.pack_into("<Q", elf_hdr, 40, sh_offset)

    sh_table = bytearray()
    for i, sec in enumerate(sections):
        hdr = bytearray(64)
        struct.pack_into("<I", hdr, 0, new_sh_names[i])
        struct.pack_into("<I", hdr, 4, sec["sh_type"])
        struct.pack_into("<Q", hdr, 8, sec["sh_flags"])
        struct.pack_into("<Q", hdr, 16, sec["sh_addr"])
        struct.pack_into("<Q", hdr, 24, new_offsets[i])
        sz = len(sec_data[i]) if sec["sh_type"] != 8 else sec["sh_size"]
        struct.pack_into("<Q", hdr, 32, sz)
        struct.pack_into("<I", hdr, 40, sec["sh_link"])
        struct.pack_into("<I", hdr, 44, sec["sh_info"])
        struct.pack_into("<Q", hdr, 48, sec["sh_addralign"])
        struct.pack_into("<Q", hdr, 56, sec["sh_entsize"])
        sh_table += hdr

    return elf_hdr + body + sh_table


def _rename_gcc_same_length(obj_path, rename_map, output_path):
    """Fast path: same-length GCC LTO rename."""
    with open(obj_path, "rb") as f:
        data = bytearray(f.read())

    sections, sec_names, sec_data, shstrtab, e_shstrndx = _gcc_lto_parse_elf_sections(data)
    renamed_count = 0

    for old_name, new_name in rename_map.items():
        old_b = old_name.encode("ascii")
        new_b = new_name.encode("ascii")

        for i, name in enumerate(sec_names):
            if ".symtab" in name and "lto" in name and "ext" not in name:
                symtab = sec_data[i]
                old_null = old_b + b"\x00"
                new_null = new_b + b"\x00"
                pos = symtab.find(old_null)
                if pos != -1:
                    symtab[pos:pos + len(old_null)] = new_null
                    renamed_count += 1

        search = b".gnu.lto_" + old_b + b"."
        pos = 0
        while True:
            pos = shstrtab.find(search, pos)
            if pos == -1:
                break
            replace_at = pos + 9
            shstrtab[replace_at:replace_at + len(old_b)] = new_b
            pos = replace_at + len(new_b)

        for i, name in enumerate(sec_names):
            if ".gnu.lto_" not in name:
                continue
            if len(sec_data[i]) < 4 or sec_data[i][:4] != b"\x28\xb5\x2f\xfd":
                continue
            try:
                decompressed = bytearray(_zstd_decompress(bytes(sec_data[i])))
            except Exception:
                continue
            if len(decompressed) < 8:
                continue
            string_size = struct.unpack_from("<I", decompressed, 4)[0]
            total = len(decompressed)
            str_start = total - string_size
            if str_start < 8 or string_size == 0:
                continue
            modified = False
            pos = str_start
            while pos < total:
                entry_len = decompressed[pos]
                if pos + 1 + entry_len > total:
                    break
                if bytes(decompressed[pos + 1:pos + 1 + entry_len]) == old_b:
                    decompressed[pos + 1:pos + 1 + entry_len] = new_b
                    modified = True
                pos += 1 + entry_len
            if modified:
                sec_data[i] = bytearray(_zstd_compress(bytes(decompressed)))

    sec_data[e_shstrndx] = shstrtab
    result = _gcc_lto_rebuild_elf(data, sections, sec_names, sec_data, shstrtab, e_shstrndx, rename_map)
    with open(output_path, "wb") as f:
        f.write(result)
    return renamed_count


def _gcc_lto_find_decls_string_ref(main_body, old_ref1, new_ref1, sec_data, sec_idx,
                                   decompressed, main_start, strtab, sections, shstrtab,
                                   e_shstrndx, sec_names, rename_map, data):
    """Find the correct byte position of a string ref using lto-dump validation."""
    old_enc = _encode_uleb128(old_ref1)
    positions = []
    p = 0
    while True:
        p = main_body.find(old_enc, p)
        if p == -1:
            break
        positions.append(p)
        p += len(old_enc)

    if len(positions) <= 1:
        return positions

    new_enc = _encode_uleb128(new_ref1)
    if len(old_enc) != len(new_enc):
        return positions[:1]

    valid = []
    for candidate in positions:
        test_body = bytearray(main_body)
        test_body[candidate:candidate + len(old_enc)] = new_enc

        test_header = bytearray(decompressed[:main_start])
        struct.pack_into("<I", test_header, 0, len(test_body))
        struct.pack_into("<I", test_header, 4, len(strtab))
        test_decomp = test_header + test_body + strtab

        test_sec_data = list(sec_data)
        test_sec_data[sec_idx] = bytearray(_zstd_compress(bytes(test_decomp)))

        test_elf = _gcc_lto_rebuild_elf(data, sections, sec_names, test_sec_data,
                                        shstrtab, e_shstrndx, rename_map)

        with tempfile.NamedTemporaryFile(suffix='.o', delete=False) as f:
            f.write(test_elf)
            test_path = f.name
        try:
            r = subprocess.run(["lto-dump", "-list", test_path],
                               capture_output=True, text=True, timeout=10)
            if r.returncode == 0:
                valid.append(candidate)
        except (subprocess.TimeoutExpired, FileNotFoundError):
            pass
        finally:
            os.unlink(test_path)

    return valid if valid else positions[:1]


def _rename_gcc_variable_length(obj_path, rename_map, output_path):
    """Variable-length GCC LTO rename with .decls string table patching."""
    with open(obj_path, "rb") as f:
        data = bytearray(f.read())

    sections, sec_names, sec_data, shstrtab, e_shstrndx = _gcc_lto_parse_elf_sections(data)

    for i, name in enumerate(sec_names):
        if ".symtab" in name and "lto" in name and "ext" not in name:
            symtab = sec_data[i]
            for old, new in rename_map.items():
                old_b = old.encode() + b"\x00"
                new_b = new.encode() + b"\x00"
                pos = symtab.find(old_b)
                if pos != -1:
                    symtab = symtab[:pos] + bytearray(new_b) + symtab[pos + len(old_b):]
            sec_data[i] = symtab

    for old, new in rename_map.items():
        search = b".gnu.lto_" + old.encode() + b"."
        pos = 0
        while True:
            pos = shstrtab.find(search, pos)
            if pos == -1:
                break
            replace_at = pos + 9
            shstrtab = shstrtab[:replace_at] + bytearray(new.encode()) + shstrtab[replace_at + len(old):]
            pos = replace_at + len(new)
    sec_data[e_shstrndx] = shstrtab

    for i, name in enumerate(sec_names):
        if ".decls" not in name:
            continue
        if len(sec_data[i]) < 4 or sec_data[i][:4] != b"\x28\xb5\x2f\xfd":
            continue

        decompressed = bytearray(_zstd_decompress(bytes(sec_data[i])))
        if len(decompressed) < 16:
            continue
        main_size = struct.unpack_from("<I", decompressed, 0)[0]
        string_size = struct.unpack_from("<I", decompressed, 4)[0]
        if string_size == 0:
            continue
        decl_state_size = struct.unpack_from("<I", decompressed, 8)[0]
        main_start = 16 + decl_state_size
        main_body = bytearray(decompressed[main_start:main_start + main_size])
        strtab = bytearray(decompressed[main_start + main_size:])

        entries = []
        pos = 0
        while pos < len(strtab):
            lpos = pos
            length = 0
            shift = 0
            while pos < len(strtab):
                byte = strtab[pos]
                pos += 1
                length |= (byte & 0x7f) << shift
                if (byte & 0x80) == 0:
                    break
                shift += 7
            if length == 0:
                entries.append((lpos, b""))
                pos = lpos + 1
                continue
            if pos + length > len(strtab):
                break
            content = bytes(strtab[pos:pos + length])
            entries.append((lpos, content))
            pos += length

        to_rename = []
        for lpos, content in entries:
            try:
                text = content.decode("ascii")
            except (UnicodeDecodeError, ValueError):
                continue
            if text in rename_map:
                to_rename.append((lpos, content, rename_map[text].encode("ascii")))

        if not to_rename:
            continue

        to_rename.sort(key=lambda x: x[0])
        appended_refs = {}

        for entry_pos, old_content, new_content in reversed(to_rename):
            old_len_enc = _encode_uleb128(len(old_content))
            new_len_enc = _encode_uleb128(len(new_content))
            old_entry = old_len_enc + old_content
            new_entry = new_len_enc + new_content

            if entry_pos == entries[-1][0]:
                if strtab[entry_pos:entry_pos + len(old_entry)] == bytearray(old_entry):
                    strtab = strtab[:entry_pos] + bytearray(new_entry) + strtab[entry_pos + len(old_entry):]
                    delta = len(new_entry) - len(old_entry)
                    for k in list(appended_refs.keys()):
                        appended_refs[k] += delta
            else:
                old_ref1 = entry_pos + 1
                new_ref1 = len(strtab) + 1
                strtab.extend(new_len_enc)
                strtab.extend(new_content)
                appended_refs[old_ref1] = new_ref1

        for old_ref1, new_ref1 in appended_refs.items():
            old_enc = _encode_uleb128(old_ref1)
            new_enc = _encode_uleb128(new_ref1)

            positions = []
            p = 0
            while True:
                p = main_body.find(old_enc, p)
                if p == -1:
                    break
                positions.append(p)
                p += len(old_enc)

            if len(positions) == 1:
                pos = positions[0]
                if len(old_enc) == len(new_enc):
                    main_body[pos:pos + len(old_enc)] = new_enc
                else:
                    main_body = main_body[:pos] + bytearray(new_enc) + main_body[pos + len(old_enc):]
            elif len(positions) > 1:
                valid = _gcc_lto_find_decls_string_ref(
                    main_body, old_ref1, new_ref1, sec_data, i,
                    decompressed, main_start, strtab, sections, shstrtab,
                    e_shstrndx, sec_names, rename_map, data)
                for pos in sorted(valid, reverse=True):
                    if len(old_enc) == len(new_enc):
                        main_body[pos:pos + len(old_enc)] = new_enc
                    else:
                        main_body = main_body[:pos] + bytearray(new_enc) + main_body[pos + len(old_enc):]

        header = bytearray(decompressed[:main_start])
        struct.pack_into("<I", header, 0, len(main_body))
        struct.pack_into("<I", header, 4, len(strtab))
        new_decompressed = header + main_body + strtab
        sec_data[i] = bytearray(_zstd_compress(bytes(new_decompressed)))

    result = _gcc_lto_rebuild_elf(data, sections, sec_names, sec_data, shstrtab, e_shstrndx, rename_map)
    with open(output_path, "wb") as f:
        f.write(result)
    return len(rename_map)


def rename_gcc_lto(obj_path, rename_map, output_path=None, gcc="gcc"):
    """Rename symbols in a GCC LTO slim object file.

    Modifies the LTO IR directly, preserving LTO format.
    """
    if output_path is None:
        output_path = obj_path
    if _all_same_length(rename_map):
        return _rename_gcc_same_length(obj_path, rename_map, output_path)
    return _rename_gcc_variable_length(obj_path, rename_map, output_path)


# =============================================================================
# Clang LTO (LLVM bitcode)
# =============================================================================

def _clang_lto_parse_layout(data):
    """Parse top-level block layout from LLVM bitcode.

    Returns list of (block_id, content_start_byte, content_end_byte).
    """
    if data[:4] != b'BC\xc0\xde':
        raise ValueError("Not an LLVM bitcode file")

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

    return blocks



def _clang_lto_rename_same_length(data, strtab_start, strtab_end, rename_map):
    """In-place byte overwrite for same-length Clang LTO renames.

    Scans the STRTAB block content for exact name matches and overwrites.
    Names in STRTAB are not null-terminated — they are packed contiguously.
    """
    renamed = 0
    for old_name, new_name in rename_map.items():
        old_bytes = old_name.encode("ascii")
        new_bytes = new_name.encode("ascii")
        pos = strtab_start
        hit = False
        while pos < strtab_end:
            idx = data.find(old_bytes, pos, strtab_end)
            if idx == -1:
                break
            data[idx:idx + len(old_bytes)] = new_bytes
            hit = True
            pos = idx + len(new_bytes)
        if hit:
            renamed += 1

    return data, renamed


def _clang_lto_rename_variable_length(data, rename_map, clang="clang"):
    """Variable-length rename of Clang LTO bitcode by round-tripping through
    textual IR.

    Uses `clang -x ir -S -emit-llvm` to disassemble, substitutes `@old_name`
    references identifier-boundary-aware, and reassembles with
    `clang -x ir -c -emit-llvm`. Keeps us decoupled from bitcode format
    details so it works across LLVM versions and on toolchains that do not
    ship libLLVM (notably Apple's).
    """
    import shutil
    import tempfile

    if not shutil.which(clang):
        raise RuntimeError(
            f"Clang LTO variable-length rename requires '{clang}' on PATH.")

    with tempfile.TemporaryDirectory() as td:
        bc_in = os.path.join(td, "in.bc")
        ll_mid = os.path.join(td, "mid.ll")
        bc_out = os.path.join(td, "out.bc")

        with open(bc_in, "wb") as f:
            f.write(bytes(data))

        r = subprocess.run(
            [clang, "-x", "ir", "-S", "-emit-llvm", bc_in, "-o", ll_mid],
            capture_output=True)
        if r.returncode != 0:
            raise RuntimeError(
                f"clang failed to disassemble bitcode: {r.stderr.decode(errors='replace')}")

        with open(ll_mid, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()

        renamed = 0
        ident_tail = r"(?![A-Za-z0-9_$.])"
        ident_head = r"(?<![A-Za-z0-9_$.])"
        for old, new in rename_map.items():
            pattern = ident_head + r"@" + re.escape(old) + ident_tail
            text, count = re.subn(pattern, "@" + new, text)
            if count:
                renamed += 1

        with open(ll_mid, "w", encoding="utf-8") as f:
            f.write(text)

        r = subprocess.run(
            [clang, "-x", "ir", "-c", "-emit-llvm", ll_mid, "-o", bc_out],
            capture_output=True)
        if r.returncode != 0:
            raise RuntimeError(
                f"clang failed to reassemble bitcode: {r.stderr.decode(errors='replace')}")

        with open(bc_out, "rb") as f:
            new_data = bytearray(f.read())

    # Caller works with unwrapped bitcode. Clang may emit a Mach-O-wrapped
    # .bc on Darwin; unwrap so the returned inner bitcode matches the
    # contract and is not double-wrapped on write.
    inner, _wrapper = _unwrap_bitcode(new_data)
    return inner, renamed


def _unwrap_bitcode(data):
    """Unwrap LLVM bitcode wrapper format (magic 0x0B17C0DE).
    Returns (inner_data, wrapper_header) or (data, None) if not wrapped."""
    if len(data) >= 20 and data[:4] == b'\xde\xc0\x17\x0b':
        offset = struct.unpack_from('<I', data, 8)[0]
        size = struct.unpack_from('<I', data, 12)[0]
        return bytearray(data[offset:offset + size]), bytearray(data[:offset])
    return data, None


def _rewrap_bitcode(inner_data, wrapper_header):
    """Rewrap bitcode with its wrapper header, updating the size field."""
    if wrapper_header is None:
        return inner_data
    struct.pack_into('<I', wrapper_header, 12, len(inner_data))
    return bytearray(wrapper_header) + inner_data


def rename_clang_lto(obj_path, rename_map, output_path=None, clang="clang"):
    """Rename symbols in a Clang LTO bitcode object file.

    Same-length renames use fast in-place STRTAB overwrite.
    Variable-length renames round-trip through textual IR using the clang
    binary, keeping us decoupled from bitcode format details.
    """
    if output_path is None:
        output_path = obj_path

    with open(obj_path, "rb") as f:
        raw_data = bytearray(f.read())

    data, wrapper = _unwrap_bitcode(raw_data)

    if not _all_same_length(rename_map):
        new_data, renamed = _clang_lto_rename_variable_length(data, rename_map, clang)
        with open(output_path, "wb") as f:
            f.write(_rewrap_bitcode(new_data, wrapper))
        return renamed

    blocks = _clang_lto_parse_layout(data)

    strtab_block = None
    for block_id, cs, ce in blocks:
        if block_id == 23:
            strtab_block = (cs, ce)

    if strtab_block is None:
        raise RuntimeError(f"Missing STRTAB block in {obj_path}")

    new_data, renamed = _clang_lto_rename_same_length(
        data, strtab_block[0], strtab_block[1], rename_map)

    with open(output_path, "wb") as f:
        f.write(_rewrap_bitcode(new_data, wrapper))
    return renamed


# =============================================================================
# MSVC LTCG (ANON_OBJECT / CIL format)
# =============================================================================

def _msvc_ltcg_find_sections(data):
    """Parse section table from MSVC LTCG object."""
    num_sections = struct.unpack_from('<H', data, 0x22)[0]
    sections = []
    for i in range(num_sections):
        off = 0x34 + i * 40
        name = data[off:off + 8].rstrip(b'\x00').decode('ascii')
        raw_size = struct.unpack_from('<I', data, off + 16)[0]
        raw_ptr = struct.unpack_from('<I', data, off + 20)[0]
        sections.append({
            'name': name, 'index': i, 'hdr_off': off,
            'raw_size': raw_size, 'raw_ptr': raw_ptr
        })
    return sections


def _msvc_ltcg_rename_one(data, old_name, new_name):
    """Rename a single symbol in an MSVC LTCG object."""
    old_bytes = old_name.encode('ascii') + b'\x00'
    new_bytes = new_name.encode('ascii') + b'\x00'
    extra_gl = len(new_bytes) - len(old_bytes)

    sections = _msvc_ltcg_find_sections(data)
    ptr_symtab = struct.unpack_from('<I', data, 0x28)[0]
    size_of_data = struct.unpack_from('<I', data, 28)[0]

    gl_sec = next((s for s in sections if 'cil$gl' in s['name']), None)
    in_sec = next((s for s in sections if 'cil$in' in s['name']), None)
    db_sec = next((s for s in sections if 'cil$db' in s['name']), None)

    if gl_sec is None:
        return data

    gl_start = gl_sec['raw_ptr']
    gl_search_end = gl_start + gl_sec['raw_size']
    if in_sec and in_sec['raw_ptr'] == gl_search_end:
        gl_search_end += in_sec['raw_size']

    name_pos_gl = data.find(old_bytes, gl_start, gl_search_end)
    if name_pos_gl == -1:
        return data

    name_pos_db = -1
    extra_db = 0
    db_record_header_pos = -1
    db_old_total = 0
    db_new_record = b''

    if db_sec:
        db_start = db_sec['raw_ptr']
        db_end = db_start + db_sec['raw_size'] + 4
        name_pos_db = data.find(old_bytes, db_start, db_end)
        if name_pos_db != -1:
            record_data_start = name_pos_db - 10
            db_record_header_pos = record_data_start - 2
            old_record_size = data[db_record_header_pos + 1]
            db_old_total = 2 + old_record_size

            new_content_len = 10 + len(new_name) + 1
            total_unpadded = 2 + new_content_len
            padding_needed = (4 - (total_unpadded % 4)) % 4
            new_record_size = new_content_len + padding_needed
            extra_db = (2 + new_record_size) - db_old_total

            padding_bytes = b'\xf3\xf2\xf1'[:padding_needed]
            fixed_bytes = bytes(data[record_data_start:name_pos_db])
            db_new_record = (bytes([0x0b, new_record_size]) + fixed_bytes +
                             new_name.encode('ascii') + b'\x00' + padding_bytes)

    total_extra = extra_gl + extra_db
    result = bytearray(data)

    if name_pos_db != -1 and extra_db != 0:
        result = (result[:db_record_header_pos] +
                  bytearray(db_new_record) +
                  result[db_record_header_pos + db_old_total:])

    result = (result[:name_pos_gl] +
              bytearray(new_bytes) +
              result[name_pos_gl + len(old_bytes):])

    gl_internal_off = gl_sec['raw_ptr'] + 0x36
    old_internal = struct.unpack_from('<H', result, gl_internal_off)[0]
    struct.pack_into('<H', result, gl_internal_off, old_internal + extra_gl)

    struct.pack_into('<I', result, gl_sec['hdr_off'] + 16, gl_sec['raw_size'] + extra_gl)
    if db_sec and extra_db != 0:
        struct.pack_into('<I', result, db_sec['hdr_off'] + 16, db_sec['raw_size'] + extra_db)

    for sec in sections:
        if sec['raw_ptr'] <= gl_sec['raw_ptr'] or sec['raw_ptr'] == 0:
            continue
        adjustment = extra_gl
        if db_sec and sec['raw_ptr'] > db_sec['raw_ptr']:
            adjustment += extra_db
        struct.pack_into('<I', result, sec['hdr_off'] + 20, sec['raw_ptr'] + adjustment)

    struct.pack_into('<I', result, 0x28, ptr_symtab + total_extra)
    struct.pack_into('<I', result, 28, size_of_data + total_extra)

    new_symtab = ptr_symtab + total_extra
    gl_in_st = result.find(b'.cil$gl', new_symtab)
    if gl_in_st != -1:
        struct.pack_into('<I', result, gl_in_st + 18, gl_sec['raw_size'] + extra_gl)
    if db_sec and extra_db != 0:
        db_in_st = result.find(b'.cil$db', new_symtab)
        if db_in_st != -1:
            struct.pack_into('<I', result, db_in_st + 18, db_sec['raw_size'] + extra_db)

    return result


def rename_msvc_lto(obj_path, rename_map, output_path=None):
    """Rename symbols in an MSVC LTCG object file (/GL compiled).

    Handles the ANON_OBJECT / CIL bitcode format. Supports variable-length
    renames by adjusting section sizes and offsets.
    """
    if output_path is None:
        output_path = obj_path

    with open(obj_path, 'rb') as f:
        data = bytearray(f.read())

    if not (len(data) >= 4 and data[:2] == b'\x00\x00' and data[2:4] == b'\xff\xff'):
        raise ValueError(f"{obj_path} is not an MSVC LTCG object file")

    renamed = 0
    for old_name, new_name in rename_map.items():
        old_len = len(data)
        data = _msvc_ltcg_rename_one(data, old_name, new_name)
        if len(data) != old_len or old_name != new_name:
            renamed += 1

    with open(output_path, 'wb') as f:
        f.write(data)
    return renamed


# =============================================================================
# Public API
# =============================================================================

def rename_obj(obj_path, rename_map, output_path=None, gcc="gcc", clang="clang"):
    """Auto-detect object format and rename symbols.

    Handles both LTO and regular object files across all platforms.
    Returns number of symbols renamed.
    """
    fmt = detect_obj_format(obj_path)
    if fmt == 'gcc-lto':
        return rename_gcc_lto(obj_path, rename_map, output_path, gcc)
    elif fmt == 'clang-lto':
        return rename_clang_lto(obj_path, rename_map, output_path, clang)
    elif fmt == 'msvc-lto':
        return rename_msvc_lto(obj_path, rename_map, output_path)
    elif fmt == 'elf':
        return rename_elf(obj_path, rename_map, output_path)
    elif fmt == 'macho':
        return rename_macho(obj_path, rename_map, output_path)
    elif fmt == 'coff':
        return rename_coff(obj_path, rename_map, output_path)
    else:
        raise ValueError(f"{obj_path}: unrecognized object file format")


# =============================================================================
# CLI
# =============================================================================

if __name__ == "__main__":
    import sys
    if len(sys.argv) < 3:
        print("Usage: lto_objcopy.py <obj_file> <syms_file> [output]")
        sys.exit(1)

    rename_map = {}
    with open(sys.argv[2]) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                parts = line.split(None, 1)
                if len(parts) == 2:
                    rename_map[parts[0]] = parts[1]

    out = sys.argv[3] if len(sys.argv) > 3 else None
    count = rename_obj(sys.argv[1], rename_map, out)
    print(f"Renamed {count} symbols")
