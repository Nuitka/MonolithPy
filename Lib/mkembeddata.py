#!/usr/bin/env python3
"""mp_embed blob generator with CHD perfect hashing.

Output layout (single nuitka_embed_map blob + separate nuitka_embed_data):

  nuitka_embed_map:
    [Header: 64 bytes]
    [Bloom filter: bloom_size_words * 8 bytes]
    [Seeds: num_buckets * 4 bytes]      (CHD displacement seeds)
    [Entries: num_entries * 32 bytes]   (slot-indexed by CHD)
    [Paths: paths_size bytes]           (null-terminated, referenced by entry.path_offset)

  nuitka_embed_data:
    [File contents]                     (concatenated, referenced by entry.data_pos)

Lookup (runtime):
    h = fnv1a_64(lower(path))
    if (!bloom_check(h)) return MISS
    bucket = h % num_buckets
    seed = seeds[bucket]
    slot = chd_mix(h, seed) % num_entries
    if (entries[slot].hash == h) return HIT
    return MISS

Hash equality is the only verification: 64-bit collisions are vanishingly
unlikely (we explicitly verify zero collisions in the keyset at gen time),
so we don't need an extra strcmp.
"""

import os
import sys
import struct

if __name__ == "__main__":
    try:
        sys.path.remove(os.path.abspath(os.path.dirname(sys.argv[0])))
    except ValueError:
        pass


def _resolve_zstd_compress():
    """Return a callable compress(data: bytes, level: int) -> bytes.

    Tries (in order): Python 3.14 stdlib compression.zstd, the
    third-party 'zstandard' PyPI package, and finally a ctypes shim
    over libzstd. We do not want to introduce a hard build-time
    dependency on a particular package - whatever the build host has."""
    try:
        from compression import zstd as _z
        return lambda data, level: _z.compress(data, level=level)
    except ImportError:
        pass
    try:
        import zstandard as _zstd
        def compress(data, level):
            return _zstd.ZstdCompressor(level=level).compress(data)
        return compress
    except ImportError:
        pass
    raise RuntimeError(
        "mkembeddata: no zstd implementation found. Need Python 3.14+ "
        "(compression.zstd), or the 'zstandard' PyPI package.")


zstd_compress = _resolve_zstd_compress()
ZSTD_LEVEL = 19  # max practical for static data, no realtime constraints

# ---- format constants (must match mp_embed.h) ----

ETYPE_DIRECTORY = 0
ETYPE_FILE = 1

EMAP_SIZE = 32
HEADER_SIZE = 64

# CHD parameters
BUCKETS_PER_ENTRIES_RATIO = 4   # ~4 keys per bucket on average
MAX_SEED = 1 << 31              # safety cap when searching seeds

# Bloom parameters
BLOOM_BITS_PER_ENTRY = 12
BLOOM_NUM_HASHES = 7

# Hash constants (FNV-1a 64)
FNV_OFFSET = 0xcbf29ce484222325
FNV_PRIME  = 0x100000001b3
MASK64 = (1 << 64) - 1
MASK32 = (1 << 32) - 1

# CHD seed-mix constants (splitmix64-style)
SPLITMIX1 = 0xbf58476d1ce4e5b9
SPLITMIX2 = 0x94d049bb133111eb
GOLDEN_RATIO = 0x9E3779B97F4A7C15

# ---- inputs ----
out_dir = ""
base_path = ""


# ---- helpers ----

def fnv1a_64(s: bytes) -> int:
    h = FNV_OFFSET
    for b in s:
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


def chd_mix(h: int, seed: int) -> int:
    """Secondary hash: mix the seed into the primary hash, splitmix-style."""
    x = (h ^ ((seed * GOLDEN_RATIO) & MASK64)) & MASK64
    x = ((x ^ (x >> 30)) * SPLITMIX1) & MASK64
    x = ((x ^ (x >> 27)) * SPLITMIX2) & MASK64
    x = (x ^ (x >> 31)) & MASK64
    return x


def bloom_bit_for(h: int, i: int, num_bits: int) -> int:
    mixed = (h * GOLDEN_RATIO + i * SPLITMIX1) & MASK64
    return mixed % num_bits


def mkfspath(file_path: str) -> str:
    if file_path == base_path:
        return "/"
    rel = os.path.relpath(file_path, base_path).replace('\\', '/').lower()
    path = "/" + rel
    if path.startswith("/__relative__"):
        path = "~" + path[len("/__relative__"):]
    return path


def next_prime(n: int) -> int:
    """Smallest prime >= n. Used to size CHD bucket array."""
    if n < 2:
        return 2

    def is_prime(x: int) -> bool:
        if x < 2: return False
        if x < 4: return True
        if x % 2 == 0: return False
        i = 3
        while i * i <= x:
            if x % i == 0:
                return False
            i += 2
        return True

    while not is_prime(n):
        n += 1
    return n


# ---- gen-time data structures ----

class Entry:
    __slots__ = ('vpath', 'hash', 'type', 'data_pos', 'data_size',
                 'path_offset', 'children_start', 'children_count',
                 'parent_vpath', 'fs_path')

    def __init__(self, vpath: str, type_: int, fs_path: str = None):
        self.vpath = vpath
        self.hash = fnv1a_64(vpath.encode('utf-8'))
        self.type = type_
        self.data_pos = 0
        self.data_size = 0
        self.path_offset = 0
        self.children_start = 0
        self.children_count = 0
        self.parent_vpath = None
        self.fs_path = fs_path


# ---- main pipeline ----

def collect_entries() -> list:
    """Walk base_path; return a list of Entry, with no slot/order assigned yet."""
    entries = []
    seen = set()

    for dirpath, dnames, fnames in os.walk(base_path):
        # Sort for deterministic output.
        dnames.sort()
        fnames.sort()

        vdir = mkfspath(dirpath)
        if vdir in ("/", "~"):
            # Skip the root - it's implicit.
            continue
        if vdir in seen:
            raise RuntimeError(f"duplicate directory: {vdir}")
        seen.add(vdir)

        e = Entry(vdir, ETYPE_DIRECTORY, dirpath)
        e.parent_vpath = mkfspath(os.path.dirname(dirpath))
        entries.append(e)

        for fname in fnames:
            vfile = vdir + "/" + fname.lower()
            if vfile in seen:
                raise RuntimeError(f"duplicate file: {vfile}")
            seen.add(vfile)
            ef = Entry(vfile, ETYPE_FILE, os.path.join(dirpath, fname))
            ef.parent_vpath = vdir
            entries.append(ef)

    return entries


def verify_no_hash_collisions(entries: list):
    """64-bit FNV-1a collisions on a curated keyset are astronomically rare,
    but we explicitly check at gen time. If we ever hit one we'd salt the
    hash and rebuild."""
    by_hash = {}
    for e in entries:
        if e.hash in by_hash:
            raise RuntimeError(
                f"hash collision: {e.vpath!r} and {by_hash[e.hash]!r} "
                f"both hash to 0x{e.hash:016x}. Pick a salt and retry."
            )
        by_hash[e.hash] = e.vpath


def organize_for_locality(entries: list) -> list:
    """Reorder entries so children of each directory are contiguous.
    Compute children_start / children_count per directory entry.

    The CHD slot is independent of this order - we'll rebuild a slot table
    after CHD assigns slots. But we need stable indices for the parent->
    children pointers. The trick is: we sort here, assign indices, then CHD
    only renames slots. We emit two mappings:
       (a) the CHD slot table (slot -> entry index in our sorted order)
       (b) but actually we want children indexed in CHD slot space, not
           sorted order space, since readdir starts from a slot.

    So plan: do CHD first, get slot for each entry; then build a new
    contiguous output where children of each dir are consecutive *output
    indices*. That output index is what entries[] uses, and we DROP the
    CHD slot table by making slot==output_index. Tricky.

    Simpler: lay out entries in a fixed order (BFS-by-parent), then run
    CHD over their (hash -> output_index) so the CHD slot DOES equal the
    output_index. CHD's job is to find seeds so this happens to work.

    We're going to use a slot table: entries[i] is at output index i.
    CHD computes a slot for each entry's hash; we want that slot == i.

    But that's not what CHD gives us. CHD gives a unique slot per entry,
    not a specific one.

    Resolution: we put a `slot_to_entry_index` table at lookup time.
    entries[] is laid out in BFS order. seeds + slot_to_entry_index live
    in the map blob; lookup goes:
       slot = chd(h, seeds[bucket])
       entry_idx = slot_to_entry_index[slot]
       if entries[entry_idx].hash == h -> match

    Two indirections instead of one. Acceptable - the slot table is small
    (4 bytes per entry), single cache line covers 16 entries.

    Actually, simpler still: skip slot_to_entry_index. Lay out entries[]
    in the slot order CHD gives us. For directory child lookups, store
    a separate "child_slots[]" array per directory that lists the slots
    of its children. readdir indexes into child_slots[children_start..
    children_start+children_count] to find each child's slot, then
    indexes entries[slot]. Cache-friendly enough.
    """
    # Strategy: we will lay out entries[] in CHD slot order. Each
    # directory entry will have children_start / children_count pointing
    # into a separate child_slots[] table.
    return entries


def build_chd(entries: list):
    """Compute CHD perfect hash. Returns (seeds, slot_for_entry).

    slot_for_entry[i] is the slot index assigned to entries[i].
    seeds[bucket_idx] is the displacement seed for that bucket.
    """
    n = len(entries)
    if n == 0:
        return [], []

    # ~4 entries per bucket. Use prime to spread.
    r = next_prime(max(1, (n + BUCKETS_PER_ENTRIES_RATIO - 1) // BUCKETS_PER_ENTRIES_RATIO))

    # Group entries by primary bucket.
    buckets = [[] for _ in range(r)]
    for i, e in enumerate(entries):
        buckets[e.hash % r].append(i)

    # Process largest buckets first (hardest to place).
    bucket_order = sorted(range(r), key=lambda b: -len(buckets[b]))

    seeds = [0] * r
    slot_for_entry = [0xFFFFFFFF] * n
    occupied = [False] * n

    for bucket_idx in bucket_order:
        bucket = buckets[bucket_idx]
        if not bucket:
            continue

        for seed in range(1, MAX_SEED):
            seen = {}
            ok = True
            for entry_idx in bucket:
                slot = chd_mix(entries[entry_idx].hash, seed) % n
                if occupied[slot] or slot in seen:
                    ok = False
                    break
                seen[slot] = entry_idx
            if ok:
                seeds[bucket_idx] = seed
                for slot, entry_idx in seen.items():
                    slot_for_entry[entry_idx] = slot
                    occupied[slot] = True
                break
        else:
            raise RuntimeError(f"CHD: ran out of seeds for bucket of size {len(bucket)}")

    # Sanity: every slot occupied.
    if not all(occupied):
        raise RuntimeError("CHD bug: some slots left unoccupied")

    return seeds, slot_for_entry


def build_bloom(entries: list):
    """Returns (bloom_words, num_bits)."""
    n = len(entries)
    if n == 0:
        return [], 0
    num_bits = ((n * BLOOM_BITS_PER_ENTRY + 63) // 64) * 64
    num_words = num_bits // 64
    bloom = [0] * num_words

    for e in entries:
        for i in range(BLOOM_NUM_HASHES):
            bit = bloom_bit_for(e.hash, i, num_bits)
            bloom[bit // 64] |= 1 << (bit % 64)

    return bloom, num_bits


def emit_c_array(f, name: str, blob: bytes):
    f.write(f"const unsigned char {name}[] = {{\n")
    for i in range(0, len(blob), 16):
        chunk = blob[i:i+16]
        f.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    f.write("};\n")
    f.write(f"const unsigned long {name}_len = {len(blob)};\n\n")


def main():
    entries = collect_entries()
    verify_no_hash_collisions(entries)

    # File data first (entry.data_pos depends on this).
    # Each file is compressed with zstd and stored as a self-contained
    # zstd frame; data_size is the *compressed* size. The frame's content
    # size header lets the runtime allocate the decompressed buffer
    # without storing the uncompressed length separately. Empty files
    # are stored as a zero-size empty frame is wasteful, so we treat
    # them as size 0 / pos 0 explicitly and the runtime short-circuits.
    data_blob = bytearray()
    raw_total = 0
    for e in entries:
        if e.type == ETYPE_FILE:
            with open(e.fs_path, 'rb') as fh:
                content = fh.read()
            raw_total += len(content)
            if len(content) == 0:
                e.data_pos = 0
                e.data_size = 0
            else:
                compressed = zstd_compress(content, ZSTD_LEVEL)
                e.data_pos = len(data_blob)
                e.data_size = len(compressed)
                data_blob.extend(compressed)
        else:
            e.data_pos = 0
            e.data_size = 0
    if raw_total > 0:
        ratio = len(data_blob) * 100.0 / raw_total
        sys.stderr.write(
            f"mkembeddata: compressed {raw_total:,} bytes -> "
            f"{len(data_blob):,} bytes ({ratio:.1f}%)\n")

    # Run CHD.
    seeds, slot_for_entry = build_chd(entries)
    n = len(entries)

    # Reorder entries so entries[slot] is the entry assigned to that slot.
    slot_entries = [None] * n
    for i, slot in enumerate(slot_for_entry):
        slot_entries[slot] = entries[i]

    # Map vpath -> slot for parent/child wiring.
    vpath_to_slot = {e.vpath: slot for slot, e in enumerate(slot_entries) if e}

    # Build child_slots[]: for each directory, the slots of its direct
    # children, contiguously. children_start indexes into this array.
    child_slots = []
    for slot, e in enumerate(slot_entries):
        if e.type == ETYPE_DIRECTORY:
            kids = []
            for kslot, ke in enumerate(slot_entries):
                if ke.parent_vpath == e.vpath:
                    kids.append(kslot)
            kids.sort()  # stable readdir
            e.children_start = len(child_slots)
            e.children_count = len(kids)
            child_slots.extend(kids)
        else:
            e.children_start = 0
            e.children_count = 0

    # Build paths blob.
    paths_blob = bytearray()
    for e in slot_entries:
        e.path_offset = len(paths_blob)
        paths_blob.extend(e.vpath.encode('utf-8'))
        paths_blob.append(0)

    # Build bloom filter.
    bloom_words, bloom_num_bits = build_bloom(slot_entries)

    # ---- assemble nuitka_embed_map blob ----
    #
    # Layout offsets - we compute all up front.
    bloom_offset = HEADER_SIZE
    bloom_size_bytes = len(bloom_words) * 8
    seeds_offset = bloom_offset + bloom_size_bytes
    seeds_size_bytes = len(seeds) * 4
    child_slots_offset = seeds_offset + seeds_size_bytes
    child_slots_size_bytes = len(child_slots) * 4
    entries_offset = child_slots_offset + child_slots_size_bytes
    entries_size_bytes = n * EMAP_SIZE
    paths_offset = entries_offset + entries_size_bytes
    paths_size = len(paths_blob)
    map_total_size = paths_offset + paths_size

    # Header (64 bytes):
    #   uint32 num_entries, num_buckets,
    #          bloom_size_words, bloom_num_hashes, bloom_num_bits,
    #          bloom_offset, seeds_offset, child_slots_offset,
    #          entries_offset, paths_offset, paths_size,
    #          num_child_slots, map_total_size
    #          + 12 bytes pad
    header = struct.pack(
        "<IIIIIIIIIIIII" + "12x",
        n,                              # num_entries
        len(seeds),                     # num_buckets
        len(bloom_words),               # bloom_size_words
        BLOOM_NUM_HASHES,               # bloom_num_hashes
        bloom_num_bits,                 # bloom_num_bits
        bloom_offset,                   # bloom_offset
        seeds_offset,                   # seeds_offset
        child_slots_offset,             # child_slots_offset
        entries_offset,                 # entries_offset
        paths_offset,                   # paths_offset
        paths_size,                     # paths_size
        len(child_slots),               # num_child_slots
        map_total_size,                 # map_total_size
    )
    assert len(header) == HEADER_SIZE, f"header is {len(header)} bytes, expected {HEADER_SIZE}"

    map_blob = bytearray()
    map_blob.extend(header)

    # Bloom
    for w in bloom_words:
        map_blob.extend(struct.pack("<Q", w))

    # Seeds
    for s in seeds:
        map_blob.extend(struct.pack("<I", s))

    # Child slots
    for cs in child_slots:
        map_blob.extend(struct.pack("<I", cs))

    # Entries (slot order)
    for e in slot_entries:
        # Layout (32 bytes):
        #   uint64 hash
        #   uint32 data_pos
        #   uint32 data_size
        #   uint32 children_start
        #   uint32 path_offset
        #   uint16 children_count
        #   uint16 path_len
        #   uint8  type
        #   uint8  flags
        #   uint16 pad
        path_bytes = e.vpath.encode('utf-8')
        emap_blob = struct.pack(
            "<QIIIIHHBBH",
            e.hash,
            e.data_pos,
            e.data_size,
            e.children_start,
            e.path_offset,
            e.children_count,
            len(path_bytes),
            e.type,
            0,                      # flags
            0,                      # pad
        )
        assert len(emap_blob) == EMAP_SIZE
        map_blob.extend(emap_blob)

    # Paths
    map_blob.extend(paths_blob)
    assert len(map_blob) == map_total_size

    # Write intermediate .dat files (kept for legacy/debug parity).
    with open(os.path.join(out_dir, 'map.dat'), 'wb') as f:
        f.write(map_blob)
    with open(os.path.join(out_dir, 'data.dat'), 'wb') as f:
        f.write(bytes(data_blob))

    # Emit C arrays.
    with open(os.path.join(out_dir, "mp_embed_data.c"), 'w') as f:
        f.write("/* Generated by mkembeddata.py - do not edit. */\n\n")
        emit_c_array(f, "nuitka_embed_map", bytes(map_blob))
        emit_c_array(f, "nuitka_embed_data", bytes(data_blob))


if __name__ == "__main__":
    out_dir = os.path.abspath(sys.argv[1])
    base_path = os.path.abspath(sys.argv[2])
    main()
