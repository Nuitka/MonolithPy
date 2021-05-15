#!/usr/bin/env python3

import os
import sys
if __name__ == "__main__":
    try:
        sys.path.remove(os.path.abspath(os.path.dirname(sys.argv[0])))
    except ValueError:
        pass

import struct
from ctypes import c_uint32

out_dir = ""
base_path = ""

map_format = "IIIIBII"

def hash(key: str) -> int:  # Hash Function: MurmurOAAT64
    h = 3323198485
    for char in key:
        h = c_uint32(h ^ ord(char)).value
        h = c_uint32(h * 0x5bd1e995).value
        h = c_uint32(h ^ (h >> 15)).value
    return h

def mkfspath(filePath: str) -> str:
    if filePath == base_path:
        return "/"
    path = "/" + os.path.relpath(filePath, base_path).replace('\\', '/').lower()
    if path.startswith("/__relative__"):
        path = "~" + path[len("/__relative__"):]
    return path

ETYPE_DIRECTORY = 0
ETYPE_FILE = 1

MAX_UINT32 = 2 ** 32 - 1


def main():
    data_temp_f = open(os.path.join(out_dir, 'data.dat'), 'wb')
    map_temp_f = open(os.path.join(out_dir, 'map.dat'), 'wb')

    path2mapidx = {}
    knownHashes = set()

    curr_map_idx = 0
    for dirpath, dnames, fnames in os.walk(base_path):
        curr_dir_path = mkfspath(dirpath)
        if curr_dir_path in ("/", "~"):
            continue
        path_hash = hash(curr_dir_path)
        if path_hash in knownHashes:
            print(f"Failed to pack {curr_dir_path} due to hash collision.")
            raise NotImplementedError()
        knownHashes.add(path_hash)

        parent_node = mkfspath(os.path.dirname(dirpath))
        if parent_node in path2mapidx:
            parent_node_idx = path2mapidx[parent_node]
        else:
            # No parent.
            parent_node_idx = MAX_UINT32

        path_start_pos = data_temp_f.tell()
        data_temp_f.write(curr_dir_path.encode('utf-8'))
        data_temp_f.write(b"\0")
        path_size = data_temp_f.tell() - path_start_pos

        dir_map_idx = curr_map_idx
        map_temp_f.write(struct.pack(map_format, path_hash, parent_node_idx, path_start_pos, path_size, ETYPE_DIRECTORY, data_temp_f.tell(), 0))
        curr_map_idx += 1

        path2mapidx[curr_dir_path] = dir_map_idx

        for fname in fnames:
            curr_file_path = curr_dir_path + "/" + fname.lower()
            path_hash = hash(curr_file_path)
            if path_hash in knownHashes:
                print(f"Failed to pack {curr_dir_path} due to hash collision.")
                raise NotImplementedError()
            knownHashes.add(path_hash)

            path_start_pos = data_temp_f.tell()
            data_temp_f.write(curr_file_path.encode('utf-8'))
            data_temp_f.write(b"\0")
            path_size = data_temp_f.tell() - path_start_pos

            data_start_pos = data_temp_f.tell()
            with open(os.path.join(dirpath, fname), 'rb') as f:
                data_temp_f.write(f.read())
            data_size = data_temp_f.tell() - data_start_pos

            file_map_idx = curr_map_idx
            map_temp_f.write(struct.pack(map_format, path_hash, dir_map_idx, path_start_pos, path_size, ETYPE_FILE, data_start_pos, data_size))
            curr_map_idx += 1

            path2mapidx[curr_file_path] = file_map_idx

    map_temp_f.close()
    data_temp_f.close()

    with open(os.path.join(out_dir, "np_embed_data.c"), 'w') as f:
        f.write("""const unsigned char nuitka_embed_map[] =
{
""")
        count = 0
        with open(os.path.join(out_dir, 'map.dat'), 'rb') as map_f:
            while (stream_byte := map_f.read(1)):
                if count % 16 == 0:
                    if count > 0:
                        f.write("\n")

                    f.write("   ")

                f.write(" 0x%02x," % ord(stream_byte))
                count += 1
        f.write("""
};

const unsigned long nuitka_embed_map_len = """ + str(count) + """;

const unsigned char nuitka_embed_data[] =
{
""")

        count = 0
        with open(os.path.join(out_dir, 'data.dat'), 'rb') as data_f:
            while (stream_byte := data_f.read(1)):
                if count % 16 == 0:
                    if count > 0:
                        f.write("\n")

                    f.write("   ")

                f.write(" 0x%02x," % ord(stream_byte))
                count += 1
        f.write("""
};

const unsigned long nuitka_embed_data_len = """ + str(count) + """;
""")

if __name__ == "__main__":
    out_dir = os.path.abspath(sys.argv[1])
    base_path = os.path.abspath(sys.argv[2])
    main()
