#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import sys

AR_MAGIC = b'!<arch>\n'
FAT_MAGIC_BE = b'\xca\xfe\xba\xbe'
FAT_MAGIC_LE = b'\xbe\xba\xfe\xca'
MEMBER_HEADER_SIZE = 60


def _make_unique_filename(base_path):
    """
    Generates a unique filename if the base path already exists.
    Appends '_1', '_2', etc., before the extension until an unused name is found.
    """
    if not os.path.exists(base_path):
        return base_path

    directory, filename = os.path.split(base_path)
    name, ext = os.path.splitext(filename)

    counter = 1
    while True:
        new_filename = f"{name}_{counter}{ext}"
        new_path = os.path.join(directory, new_filename)
        if not os.path.exists(new_path):
            return new_path
        counter += 1


class ArchiveMemberHeader:

    def __init__(self, header_bytes):
        if len(header_bytes) != MEMBER_HEADER_SIZE:
            raise ValueError("Header must be 60 bytes long.")

        self.raw_name = header_bytes[0:16]

        # A helper to safely parse integer fields that might be empty or malformed.
        def _safe_int_parse(byte_slice, base):
            try:
                # .strip() handles space padding. 'or b"0"' handles empty fields.
                return int(byte_slice.strip() or b"0", base)
            except ValueError:
                # This can happen if a field contains non-numeric junk, which
                # indicates a malformed header or that we are out of sync in the file.
                raise ValueError(f"Invalid literal for int() with base {base}: {byte_slice!r}")

        self.mtime = _safe_int_parse(header_bytes[16:28], 10)
        self.uid = _safe_int_parse(header_bytes[28:34], 10)
        self.gid = _safe_int_parse(header_bytes[34:40], 10)
        self.mode = _safe_int_parse(header_bytes[40:48], 8)
        self.size = _safe_int_parse(header_bytes[48:58], 10)

        self.fmag = header_bytes[58:60]

        if self.fmag != b'\x60\x0a':
            raise ValueError("Invalid member header magic value.")


def extract_archive(archive_path, output_dir="."):

    try:
        os.makedirs(output_dir, exist_ok=True)
    except OSError as e:
        print(f"Error: Could not create output directory '{output_dir}': {e}", file=sys.stderr)
        return False

    try:
        with open(archive_path, 'rb') as f:
            magic = f.read(8)
            if magic.startswith(FAT_MAGIC_BE) or magic.startswith(FAT_MAGIC_LE):
                print(f"Error: '{archive_path}' is a macOS universal ('fat') binary.", file=sys.stderr)
                print("This utility is designed for standard 'ar' archives, not fat containers.", file=sys.stderr)
                return False

            if magic != AR_MAGIC:
                print(f"Error: '{archive_path}' is not a valid 'ar' archive file.", file=sys.stderr)
                return False

            long_names_table = None
            extracted_filenames = set()

            while True:
                current_pos = f.tell()
                header_bytes = f.read(MEMBER_HEADER_SIZE)

                if not header_bytes:
                    # End of file
                    break

                if len(header_bytes) < MEMBER_HEADER_SIZE:
                    print(f"Warning: Truncated header found at offset {current_pos}. Stopping.", file=sys.stderr)
                    break

                try:
                    header = ArchiveMemberHeader(header_bytes)
                except (ValueError, IndexError) as e:
                    print(f"Error parsing member header at offset {current_pos}: {e}", file=sys.stderr)
                    return False

                filename = ""
                data_size = header.size

                # 4. Resolve filename
                raw_name_str = header.raw_name.strip().decode('ascii', 'ignore')

                if raw_name_str.startswith('#1/'):
                    # BSD-style long filename
                    try:
                        name_len = int(raw_name_str[3:])
                        filename = f.read(name_len).decode('ascii').rstrip('\x00')
                        data_size -= name_len
                    except (ValueError, IndexError):
                        print(f"Error parsing BSD-style long name at offset {current_pos}.", file=sys.stderr)
                        f.seek(header.size, 1)  # Attempt to skip to next member
                        if header.size % 2 != 0: f.seek(1, 1)
                        continue
                elif raw_name_str.startswith('/'):
                    # GNU/SysV-style long filename
                    if long_names_table is None:
                        print(f"Error: Found GNU-style long name reference at offset {current_pos} "
                              "but no long name table ('//') has been seen yet.", file=sys.stderr)
                        f.seek(header.size, 1)  # Attempt to skip to next member
                        if header.size % 2 != 0:
                            f.seek(1, 1)
                        continue
                    try:
                        offset = int(raw_name_str[1:])
                        end_offset = long_names_table.find(b'/\n', offset)
                        if end_offset == -1:
                            end_offset = len(long_names_table)  # some variants might not use the trailer
                        filename = long_names_table[offset:end_offset].decode('ascii')
                    except (ValueError, IndexError):
                        print(f"Error parsing GNU-style long name at offset {current_pos}.", file=sys.stderr)
                        f.seek(header.size, 1)  # Attempt to skip to next member
                        if header.size % 2 != 0:
                            f.seek(1, 1)
                        continue
                else:
                    # Standard short filename
                    filename = raw_name_str.rstrip('/')

                data_to_read = data_size
                if filename == '/' or filename.startswith('__.SYMDEF'):
                    # Symbol table, skip
                    f.seek(data_to_read, 1)
                elif filename == '//':
                    # GNU/SysV long names table
                    long_names_table = f.read(data_to_read)
                else:
                    if not filename:
                        print(f"Warning: Skipping member with empty name at offset {current_pos}.", file=sys.stderr)
                        f.seek(data_to_read, 1)
                    else:
                        base_output_path = os.path.join(output_dir, os.path.basename(filename))
                        output_path = _make_unique_filename(base_output_path)

                        try:
                            with open(output_path, 'wb') as out_f:
                                out_f.write(f.read(data_to_read))
                            extracted_filenames.add(filename)
                        except IOError as e:
                            print(f"Error writing file '{output_path}': {e}", file=sys.stderr)
                            # Don't halt, try to extract other members
                            f.seek(data_to_read, 1)  # Skip over data

                # 7. Advance to the next member, accounting for padding
                # Data must be padded to an even byte boundary
                if header.size % 2 != 0:
                    f.seek(1, 1)  # Skip 1 padding byte

    except FileNotFoundError:
        print(f"Error: Archive file not found at '{archive_path}'", file=sys.stderr)
        return False
    except IOError as e:
        print(f"Error reading archive file '{archive_path}': {e}", file=sys.stderr)
        return False

    print("Extraction complete.")
    return True


if __name__ == "__main__":
    if len(sys.argv) not in [2, 3]:
        print(f"Usage: {sys.argv} <archive_file> [output_directory]")
        print("Extracts all members from a static archive (.a,.lib) to the specified directory.")
        sys.exit(1)

    archive_file = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) == 3 else "."

    if not extract_archive(archive_file, output_dir):
        sys.exit(1)
