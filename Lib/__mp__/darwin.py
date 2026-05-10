# macOS is mostly the same as linux.
import os.path
from .linux import *

import hashlib


# macOS reuses linux's pyobjtools-based get_object_symbols /
# _parse_rename_arg_file. Override rename_symbols_in_object to drop the
# llvm-objcopy / clang-bitcode-conversion path: pyobjtools handles
# Mach-O and Clang LTO bitcode natively (variable-length renames in
# bitcode shell out to the system `clang`, which is always present on
# macOS - no separate install_build_tool('clang') needed).
def rename_symbols_in_object(obj_path, rename_arg_file, cwd):
    from .tools.pyobjtools import objcopy as _pyobj_objcopy
    full_path = obj_path if os.path.isabs(obj_path) else os.path.join(cwd, obj_path)
    rename_map = _parse_rename_arg_file(rename_arg_file)
    if rename_map:
        _pyobj_objcopy.rename_obj(full_path, rename_map)


# Override repack_library to use libtool instead of ar on macOS
def repack_library(lib_path, obj_list):
    """
    Repack object files into a static library using libtool.
    Matches the pattern used in existing rename_symbols_in_file.

    Args:
        lib_path: Path to the output library file
        obj_list: List of object file paths to include
    """
    subprocess.run(["libtool", "-static", "-o", lib_path] + obj_list, check=True)


def rename_symbols_in_file(target_lib, prefix, protected_symbols=[]):
    target_lib = os.path.abspath(target_lib)
    with tempfile.TemporaryDirectory() as tmpdir:
        from .tools.pyobjtools import ar as _ar
        _ar.extract_archive(target_lib, tmpdir)
        obj_list = []
        known_symbols = set()
        unmatched_symbols = set()
        keep_symbols = set()
        for obj in os.listdir(tmpdir):
            if obj.endswith(".o"):
                obj_path = os.path.join(tmpdir, obj)
                obj_list.append(obj_path)
                symbol_data = get_object_symbols(obj_path) or []
                obj_symbols = [(x[x.rindex(' ') + 1:], x) for x in symbol_data if len(x) > 3]
                obj_symbols = [x for x in obj_symbols if not x[0].startswith(".")]
                for sym in obj_symbols:
                    if any(re.fullmatch(x, sym[0]) for x in protected_symbols):
                        keep_symbols.add(sym[0])
                    if sym[0].startswith("PyInit_") or sym[0].startswith("_PyInit_") or (not re.fullmatch(r"[a-zA-Z0-9_-]+", sym[0]) and "pybind" not in sym[0]):
                        keep_symbols.add(sym[0])
                    if ' u ' in sym[1].lower():
                        unmatched_symbols.add(sym[0])
                    else:
                        known_symbols.add(sym[0])

        unmatched_symbols = unmatched_symbols - known_symbols
        rename_map = {sym: prefix + sym for sym in (known_symbols - unmatched_symbols - keep_symbols)}

        if rename_map:
            from .tools.pyobjtools import objcopy as _pyobj_objcopy
            for obj in obj_list:
                _pyobj_objcopy.rename_obj(obj, rename_map)

        os.rename(target_lib, target_lib + ".orig")
        subprocess.run(["libtool", "-static", "-o", target_lib] + obj_list)


def rename_init_symbol_in_file(target_lib):
    target_lib = os.path.abspath(target_lib)
    with tempfile.TemporaryDirectory() as tmpdir:
        hasher = hashlib.md5()
        with open(target_lib, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                hasher.update(chunk)
        file_hash = hasher.hexdigest()
        from .tools.pyobjtools import ar as _ar
        _ar.extract_archive(target_lib, tmpdir)
        obj_list = []

        from .tools.pyobjtools import objcopy as _pyobj_objcopy

        for obj in os.listdir(tmpdir):
            if obj.endswith(".o"):
                obj_path = os.path.join(tmpdir, obj)
                obj_list.append(obj_path)
                symbols_to_rename = set()

                symbol_data = get_object_symbols(obj_path) or []
                obj_symbols = [(x[x.rindex(' ') + 1:], x) for x in symbol_data if len(x) > 3]
                obj_symbols = [x for x in obj_symbols if not x[0].startswith(".")]
                for sym in obj_symbols:
                    if sym[0].startswith("PyInit_") or sym[0].startswith("_PyInit_") or \
                            "pyx_CommonTypesMetaclass" in sym[0] or \
                            "f2pyinittypes" in sym[0] or \
                            ("pybind11" in sym[0] and '@' not in sym[0] and '?' not in sym[0]):
                        symbols_to_rename.add(sym[0])

                if not symbols_to_rename:
                    continue

                rename_map = {sym: f"{sym}__mp__{file_hash}" for sym in symbols_to_rename}
                for old_sym, new_sym in rename_map.items():
                    print(f"Renaming {old_sym} to {new_sym}")
                _pyobj_objcopy.rename_obj(obj_path, rename_map)

        os.rename(target_lib, target_lib + ".orig")
        subprocess.run(["libtool", "-static", "-o", target_lib] + obj_list)


def remove_symbols_in_file(target_lib, object_file, symbols):
    """See linux.remove_symbols_in_file for the rationale on the
    rename-to-unused-name strategy."""
    with tempfile.TemporaryDirectory() as tmpdir:
        from .tools.pyobjtools import ar as _ar
        _ar.extract_archive(target_lib, tmpdir)

        obj_list = [os.path.join(tmpdir, x) for x in os.listdir(tmpdir) if x.endswith(".o")]

        target_obj_path = os.path.join(tmpdir, object_file)
        if symbols and os.path.exists(target_obj_path):
            from .tools.pyobjtools import objcopy as _pyobj_objcopy
            rename_map = {sym: f"__mp_stripped_{hashlib.md5(sym.encode()).hexdigest()}" for sym in symbols}
            _pyobj_objcopy.rename_obj(target_obj_path, rename_map)

        os.rename(target_lib, target_lib + ".orig")
        subprocess.run(["libtool", "-static", "-o", target_lib] + obj_list)

