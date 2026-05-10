"""Smoke test for a freshly built MonolithPy interpreter.

Exercises the Python stdlib + native extension modules that come with
the build (ctypes, ssl, sqlite3, _zstd, _remote_debugging, etc.) plus
3.14-specific features (subinterpreters, compression.zstd).

Intended to be run by CI as `built_python tests/python_smoke.py` after
the build artifact is produced. Exits non-zero on failure.
"""

from __future__ import annotations

import importlib
import sys
import traceback


fails: list[tuple[str, str]] = []
passes = 0


def expect(name: str, fn):
    global passes
    try:
        fn()
        passes += 1
    except BaseException as exc:  # noqa: BLE001 - CI smoke test, want everything
        fails.append((name, traceback.format_exc()))


def test_version():
    assert sys.version_info[:2] >= (3, 13), sys.version_info


def test_stdlib_imports():
    # Pure-Python and built-in stdlib modules. If any of these can't load,
    # there's a serious build problem.
    modules = [
        "os", "sys", "io", "json", "re", "collections", "itertools",
        "functools", "pathlib", "subprocess", "threading", "queue",
        "asyncio", "concurrent.futures", "socket", "urllib.request",
        "http.client", "email", "xml.etree.ElementTree", "csv",
        "configparser", "argparse", "logging", "unittest", "pickle",
        "shelve", "secrets", "base64", "codecs", "struct",
        "datetime", "calendar", "time", "random", "statistics",
        "fractions", "math", "cmath", "platform", "sysconfig",
        "tempfile", "shutil", "glob", "fnmatch",
        "string", "textwrap", "unicodedata", "difflib",
        "typing", "dataclasses", "enum", "abc", "contextlib",
        "heapq", "bisect", "weakref", "copy", "copyreg",
        "inspect", "dis", "ast", "tokenize", "symtable",
        "compileall", "py_compile", "pyclbr",
        "pdb", "profile", "cProfile", "pstats", "timeit",
        "gc", "tracemalloc", "faulthandler", "warnings",
        "wsgiref", "smtplib", "imaplib",
        "gettext", "locale",
    ]
    for m in modules:
        importlib.import_module(m)


def test_native_modules():
    # C extension modules MonolithPy ships statically linked.
    natives = [
        "ctypes", "_ctypes",
        "ssl", "_ssl",
        "hashlib", "_hashlib", "_blake2", "_md5", "_sha1", "_sha2", "_sha3", "_hmac",
        "sqlite3", "_sqlite3",
        "zlib", "bz2", "lzma", "_bz2", "_lzma",
        "_decimal", "_socket", "_ssl",
        "select", "mmap", "_uuid",
        "_zoneinfo",
        "_pickle", "_json", "_csv",
        "unicodedata", "_elementtree", "pyexpat",
        "_multiprocessing",
    ]
    for m in natives:
        importlib.import_module(m)


def test_ctypes_basics():
    import ctypes
    # POINTER moved from _ctypes (C) to Lib/ctypes/__init__.py (Python) in 3.14.
    assert callable(ctypes.POINTER), ctypes.POINTER
    assert ctypes.pythonapi is None or hasattr(ctypes.pythonapi, "_handle")
    arr = (ctypes.c_int * 5)(1, 2, 3, 4, 5)
    assert list(arr) == [1, 2, 3, 4, 5]


def test_ssl_default_context():
    # Implicitly verifies the SSL cert is reachable through the embed.
    import ssl
    ctx = ssl.create_default_context()
    assert ctx.protocol == ssl.PROTOCOL_TLS_CLIENT


def test_sqlite_in_memory():
    import sqlite3
    conn = sqlite3.connect(":memory:")
    conn.execute("CREATE TABLE t (a INTEGER, b TEXT)")
    conn.executemany("INSERT INTO t VALUES (?, ?)", [(1, "a"), (2, "b")])
    rows = list(conn.execute("SELECT a, b FROM t ORDER BY a"))
    assert rows == [(1, "a"), (2, "b")], rows
    conn.close()


def test_compression_modules():
    import zlib, bz2, lzma
    payload = b"abc" * 1000
    for mod_name, mod in (("zlib", zlib), ("bz2", bz2), ("lzma", lzma)):
        c = mod.compress(payload)
        assert mod.decompress(c) == payload, mod_name


def test_compression_zstd_3_14():
    # New in 3.14.
    if sys.version_info[:2] < (3, 14):
        return
    from compression import zstd
    payload = b"the quick brown fox jumps over the lazy dog" * 100
    c = zstd.compress(payload)
    assert zstd.decompress(c) == payload


def test_subinterpreters_3_14():
    # PEP 554 stable interpreters API.
    if sys.version_info[:2] < (3, 14):
        return
    from concurrent import interpreters
    i = interpreters.create()
    try:
        i.exec("x = 2 + 3")
    finally:
        i.close()


def test_threading_basic():
    import threading
    out = []
    def worker():
        out.append(threading.get_ident())
    t = threading.Thread(target=worker)
    t.start(); t.join()
    assert len(out) == 1


def test_asyncio_run():
    import asyncio
    async def main():
        await asyncio.sleep(0)
        return 42
    assert asyncio.run(main()) == 42


def test_pickle_roundtrip():
    import pickle
    obj = {"a": [1, 2, 3], "b": (4, 5), "c": {"x", "y"}}
    blob = pickle.dumps(obj)
    assert pickle.loads(blob) == obj


def test_hashlib_sha256():
    import hashlib
    h = hashlib.sha256(b"hello").hexdigest()
    assert h == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824", h


def test_remote_debugging_module_3_14():
    # Built-in module added in 3.14.
    if sys.version_info[:2] < (3, 14):
        return
    import _remote_debugging
    # Smoke - just verify it loaded.
    assert _remote_debugging is not None


def test_subprocess_python():
    import subprocess
    r = subprocess.run(
        [sys.executable, "-c", "print(2 + 2)"],
        capture_output=True, text=True, check=True,
    )
    assert r.stdout.strip() == "4", r


def main():
    tests = [
        ("version", test_version),
        ("stdlib_imports", test_stdlib_imports),
        ("native_modules", test_native_modules),
        ("ctypes_basics", test_ctypes_basics),
        ("ssl_default_context", test_ssl_default_context),
        ("sqlite_in_memory", test_sqlite_in_memory),
        ("compression_modules", test_compression_modules),
        ("compression_zstd_3_14", test_compression_zstd_3_14),
        ("subinterpreters_3_14", test_subinterpreters_3_14),
        ("threading_basic", test_threading_basic),
        ("asyncio_run", test_asyncio_run),
        ("pickle_roundtrip", test_pickle_roundtrip),
        ("hashlib_sha256", test_hashlib_sha256),
        ("remote_debugging_module_3_14", test_remote_debugging_module_3_14),
        ("subprocess_python", test_subprocess_python),
    ]
    for name, fn in tests:
        expect(name, fn)

    print(f"=== Python smoke: {passes} pass, {len(fails)} fail ===")
    for name, tb in fails:
        sys.stderr.write(f"--- FAIL {name} ---\n{tb}\n")
    sys.exit(0 if not fails else 1)


if __name__ == "__main__":
    main()
