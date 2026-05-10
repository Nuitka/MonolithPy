#!/usr/bin/env python3
# Part of pyobjtools — https://github.com/Maxwell175/pyobjtools
# Copyright (c) 2026 Maxwell Dreytser
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""
Cross-platform ar/lib archive extractor (pure Python).

Extracts member objects from Unix ar archives (.a) and Windows static
libraries (.lib). Handles BSD-style and GNU/SysV-style long filenames.
"""
import os
import sys

AR_MAGIC = b'!<arch>\n'
FAT_MAGIC_BE = b'\xca\xfe\xba\xbe'
FAT_MAGIC_LE = b'\xbe\xba\xfe\xca'
_MEMBER_HEADER_SIZE = 60


def _make_unique_filename(base_path):
    if not os.path.exists(base_path):
        return base_path
    directory, filename = os.path.split(base_path)
    name, ext = os.path.splitext(filename)
    counter = 1
    while True:
        new_path = os.path.join(directory, f"{name}_{counter}{ext}")
        if not os.path.exists(new_path):
            return new_path
        counter += 1


def _parse_member_header(header_bytes):
    if len(header_bytes) != _MEMBER_HEADER_SIZE:
        raise ValueError("Header must be 60 bytes")

    fmag = header_bytes[58:60]
    if fmag != b'\x60\x0a':
        raise ValueError("Invalid member header magic")

    def parse_int(s, base):
        s = s.strip()
        return int(s or b"0", base)

    return {
        "raw_name": header_bytes[0:16],
        "size": parse_int(header_bytes[48:58], 10),
    }


def extract_archive(archive_path, output_dir="."):
    """Extract all object members from an ar archive.

    Returns list of extracted filenames, or raises on error.
    """
    with open(archive_path, 'rb') as f:
        magic = f.read(8)

        if magic.startswith(FAT_MAGIC_BE) or magic.startswith(FAT_MAGIC_LE):
            raise ValueError(f"{archive_path}: macOS fat binary, not an ar archive")

        if magic != AR_MAGIC:
            raise ValueError(f"{archive_path}: not a valid ar archive")

        long_names_table = None
        extracted = []

        while True:
            header_bytes = f.read(_MEMBER_HEADER_SIZE)
            if not header_bytes:
                break
            if len(header_bytes) < _MEMBER_HEADER_SIZE:
                break

            header = _parse_member_header(header_bytes)
            raw_name = header["raw_name"].strip().decode('ascii', 'ignore')
            size = header["size"]

            if raw_name == '/':
                f.seek(size + (size % 2), 1)
                continue

            if raw_name == '//':
                long_names_table = f.read(size)
                if size % 2 != 0:
                    f.seek(1, 1)
                continue

            filename = ""
            data_size = size

            if raw_name.startswith('#1/'):
                name_len = int(raw_name[3:])
                filename = f.read(name_len).decode('ascii').rstrip('\x00')
                data_size -= name_len
            elif raw_name.startswith('/') and long_names_table is not None:
                offset = int(raw_name[1:])
                end = long_names_table.find(b'/\n', offset)
                if end == -1:
                    end = long_names_table.find(b'\x00', offset)
                if end == -1:
                    end = len(long_names_table)
                filename = long_names_table[offset:end].decode('ascii')
            else:
                filename = raw_name.rstrip('/')

            if filename.startswith('__.SYMDEF') or not filename:
                f.seek(data_size, 1)
            else:
                safe_name = os.path.basename(filename)
                output_path = _make_unique_filename(os.path.join(output_dir, safe_name))
                os.makedirs(output_dir, exist_ok=True)
                with open(output_path, 'wb') as out_f:
                    out_f.write(f.read(data_size))
                extracted.append(output_path)

            if size % 2 != 0:
                f.seek(1, 1)

    return extracted


# =============================================================================
# CLI
# =============================================================================

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: extract_ar.py <archive_file> [output_directory]", file=sys.stderr)
        sys.exit(1)

    archive_file = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else "."

    try:
        files = extract_archive(archive_file, output_dir)
        for f in files:
            print(f)
    except (ValueError, OSError) as e:
        print(f"extract_ar: {e}", file=sys.stderr)
        sys.exit(1)
