"""Generate a torture-test embed tree under tests/mp_embed/tree/.

Files are organised under /c/<...> so that on Windows fopen("C:\\<...>")
hits them, and on Linux fopen("/c/<...>") hits them. (mp_embed maps
Windows drive letter <X>:\foo to virtual path /<x>/foo.)
"""

import hashlib
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "tree"))


def write(rel_path, data):
    full = os.path.join(ROOT, rel_path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    if isinstance(data, str):
        data = data.encode("utf-8")
    with open(full, "wb") as f:
        f.write(data)


def main():
    # Wipe tree
    if os.path.isdir(ROOT):
        import shutil
        shutil.rmtree(ROOT)
    os.makedirs(ROOT)

    # Files under "C/embedtest/" so virtual paths become /c/embedtest/...
    base = "C/embedtest"

    # Edge-case content sizes
    write(f"{base}/empty.txt", b"")
    write(f"{base}/one.bin", b"\x42")
    write(f"{base}/small.txt", "Hello, World!\n")

    # All-bytes binary
    write(f"{base}/binary.bin", bytes(range(256)))

    # ~1 KB lorem
    write(f"{base}/lorem.txt", "Lorem ipsum dolor sit amet, consectetur adipiscing elit. " * 20 + "\n")

    # 1 MB pseudo-random (deterministic). Incompressible by design - exercises
    # the worst case for the zstd path.
    big = bytearray()
    h = hashlib.sha256(b"seed").digest()
    while len(big) < 1024 * 1024:
        big.extend(h)
        h = hashlib.sha256(h).digest()
    write(f"{base}/huge.dat", bytes(big[:1024 * 1024]))

    # 256 KB highly compressible payload - exercises the best case and lets
    # the test suite verify the zstd round-trip on a non-trivial file.
    write(f"{base}/compressible.dat", b"the quick brown fox jumps over the lazy dog. " * 6000)

    # Funky filenames
    write(f"{base}/name with spaces.txt", "spaces work\n")
    write(f"{base}/UPPERCASE.TXT", "case folding test\n")
    write(f"{base}/multiple.dots.in.name", "dots\n")

    # Deep nesting
    write(f"{base}/deep/nested/tree/leaf.txt", "found it\n")
    write(f"{base}/deep/nested/sibling.txt", "sibling at depth 2\n")
    write(f"{base}/deep/cousin.txt", "cousin at depth 1\n")

    # Many files in one directory (test dir iteration scaling)
    for i in range(50):
        write(f"{base}/manyfiles/file_{i:02d}.txt", f"file index {i}\n")

    # Long path: ~200 chars
    long_segment = "abcdefghijklmnopqrstuvwxyz0123456789"
    long_path = f"{base}/longpath/{long_segment}/{long_segment}/{long_segment}/leaf.txt"
    write(long_path, "long path content\n")

    # Top-level too
    write(f"{base}/readme.md", "Test tree readme\n")

    print(f"wrote test tree under {ROOT}")


if __name__ == "__main__":
    main()
