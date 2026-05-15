import hashlib
from .common import *


def get_compiler_module():
    __import__("setuptools._distutils.unixccompiler")
    return sys.modules["setuptools._distutils.unixccompiler"]


def make(*args):
    return run_with_output("make", *args)


def auto_patch_build_file(fpath):
    try:
        if fpath.endswith("CMakeLists.txt"):
            with open(fpath, "r") as f:
                s = f.read()
            s2 = re.sub(
                r"cmake_minimum_required *\( *VERSION [0-9\.]+ *\)",
                f"""cmake_minimum_required(VERSION 3.15)

add_link_options({os.path.join(sysconfig.get_config_var('LIBDIR'), 'libmp_embed.a')})
include_directories({sysconfig.get_config_var("INCLUDEPY")})
""",
                s,
                flags=re.IGNORECASE,
            )
            if s != s2:
                my_print("Fixed up file: %s" % fpath, style="blue")
                with open(fpath, "w") as f:
                    f.write(s2)
    except Exception:
        pass


def auto_patch_build(folder):
    for dname, dirs, files in os.walk(folder):
        for fname in files:
            fpath = os.path.join(dname, fname)
            if ".git" in fpath or ".svn" in fpath:
                continue

            auto_patch_build_file(fpath)


def get_object_symbols(obj):
    """Return llvm-nm-style ' <type> <name>' lines for `obj`.

    Implemented via pyobjtools so we don't shell out to /usr/bin/nm — that
    keeps the symbol parser working uniformly across plain ELF objects and
    the GCC-LTO slim objects produced by `-flto -fno-fat-lto-objects`."""
    try:
        from .tools.pyobjtools import nm as _pyobj_nm
        return [s.format() for s in _pyobj_nm.nm(obj)]
    except Exception:
        return None


def _parse_rename_arg_file(rename_arg_file):
    """Read a `--redefine-syms` style rename file into a dict."""
    mapping = {}
    with open(rename_arg_file, "r") as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) == 2:
                mapping[parts[0]] = parts[1]
    return mapping


def is_object_file(filename):
    """Check if a file is an object file for this platform."""
    return filename.endswith(".o")


def is_undefined_symbol(symbol_line):
    """Check if a symbol line from nm represents an undefined symbol."""
    # Match the pattern used in rename_symbols_in_file
    return ' U ' in symbol_line or symbol_line.strip().startswith('U ')


def is_local_symbol(symbol_line):
    """Check if a symbol line from nm represents a local (non-global) symbol.

    In nm output:
    - Uppercase letters (T, D, B, etc.) = global symbols
    - Lowercase letters (t, d, b, etc.) = local symbols
    - U = undefined (not local, not global)
    """
    line = symbol_line.strip()
    # Skip undefined symbols
    if ' U ' in symbol_line or line.startswith('U '):
        return False
    # Look for the symbol type character (single letter between spaces)
    # Format is typically: "address type name" or "type name"
    parts = line.split()
    if len(parts) >= 2:
        # The type is usually the second-to-last element before the symbol name
        # or the first element if there's no address
        for part in parts[:-1]:  # Exclude the symbol name (last part)
            if len(part) == 1 and part.isalpha():
                # Found the type character - lowercase means local
                return part.islower()
    return False


def extract_archive_subprocess(lib_path, output_dir):
    """Extract a static library archive via the vendored pyobjtools.ar."""
    try:
        from .tools.pyobjtools import ar as _ar
        _ar.extract_archive(lib_path, output_dir)
        return True
    except Exception as e:
        my_print(f"Failed to extract {lib_path}: {e}", style="red")
        return False


def rename_symbols_in_object(obj_path, rename_arg_file, cwd):
    """
    Rename symbols in an object file using a rename arguments file.

    Routes through pyobjtools, which detects ELF, GCC-LTO slim, Clang LTO
    bitcode, etc. transparently — so the previous explicit
    "if LLVM bitcode then clang -c" pre-step (and the LLVM toolchain it
    needed) is no longer required.
    """
    from .tools.pyobjtools import objcopy as _pyobj_objcopy
    rename_map = _parse_rename_arg_file(rename_arg_file)
    if rename_map:
        _pyobj_objcopy.rename_obj(obj_path, rename_map)


def repack_library(lib_path, obj_list):
    """
    Repack object files into a static library using ar.
    Uses basenames and cwd to match existing pattern.

    Args:
        lib_path: Path to the output library file
        obj_list: List of object file paths to include
    """
    # Extract directory from first object file (they should all be in same dir)
    if obj_list:
        cwd = os.path.dirname(obj_list[0])
        basenames = [os.path.basename(o) for o in obj_list]
        run("ar", "rcs", lib_path, *basenames, cwd=cwd)
    else:
        # Fallback if no objects
        run("ar", "rcs", lib_path)


def rename_symbols_in_file(target_lib, suffix, protected_symbols=[]):
    target_lib = os.path.abspath(target_lib)
    with tempfile.TemporaryDirectory() as tmpdir:
        from .tools.pyobjtools import ar as _ar
        _ar.extract_archive(target_lib, tmpdir)
        obj_list = []
        known_symbols = set()
        unmatched_symbols = set()
        keep_symbols = set()

        for obj_filename in os.listdir(tmpdir):
            if obj_filename.endswith(".o"):
                obj_path = os.path.join(tmpdir, obj_filename)
                obj_list.append(obj_path)
                symbol_data = get_object_symbols(obj_path) or []
                obj_symbols = [(x[x.rindex(' ') + 1:], x) for x in symbol_data if ' ' in x and len(x) > 3]
                obj_symbols = [s for s in obj_symbols if not s[0].startswith(".")]
                for sym_name, full_line in obj_symbols:
                    if any(re.fullmatch(pattern, sym_name) for pattern in protected_symbols):
                        keep_symbols.add(sym_name)
                    if sym_name.startswith("PyInit_") or sym_name.startswith("_PyInit_") or (not re.fullmatch(r"[a-zA-Z0-9_-]+", sym_name) and "pybind" not in sym_name):
                        keep_symbols.add(sym_name)
                    if ' U ' in full_line or full_line.strip().startswith('U '):
                        unmatched_symbols.add(sym_name)
                    else:
                        known_symbols.add(sym_name)

        rename_map = {
            sym: sym + suffix
            for sym in (known_symbols - unmatched_symbols - keep_symbols)
        }

        if rename_map:
            from .tools.pyobjtools import objcopy as _pyobj_objcopy
            for obj_p in obj_list:
                _pyobj_objcopy.rename_obj(obj_p, rename_map)

        os.rename(target_lib, target_lib + ".orig")
        # Use argument unpacking for the list of object files
        run("ar", "rcs", target_lib, *[os.path.basename(o) for o in obj_list], cwd=tmpdir)


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
        obj_list_paths = []
        obj_filenames = []

        from .tools.pyobjtools import objcopy as _pyobj_objcopy

        for obj_filename in os.listdir(tmpdir):
            if obj_filename.endswith(".o"):
                obj_path = os.path.join(tmpdir, obj_filename)
                obj_list_paths.append(obj_path)
                obj_filenames.append(obj_filename)

                symbols_to_rename_map = {}

                symbol_data = get_object_symbols(obj_path) or []
                obj_symbols = [(x[x.rindex(' ') + 1:], x) for x in symbol_data if ' ' in x and len(x) > 3]
                obj_symbols = [s for s in obj_symbols if not s[0].startswith(".")]

                for sym_name, full_line in obj_symbols:
                    if (sym_name.startswith("PyInit_") or sym_name.startswith("_PyInit_") or
                            "pyx_CommonTypesMetaclass" in sym_name or
                            "f2pyinittypes" in sym_name or
                            ("pybind11" in sym_name and '@' not in sym_name and '?' not in sym_name)) and \
                            not (' U ' in full_line or full_line.strip().startswith('U ')):
                        symbols_to_rename_map[sym_name] = f"{sym_name}__mp__{file_hash}"

                if not symbols_to_rename_map:
                    continue

                for old_sym, new_sym in symbols_to_rename_map.items():
                    print(f"Renaming {old_sym} to {new_sym}")
                _pyobj_objcopy.rename_obj(obj_path, symbols_to_rename_map)

        os.rename(target_lib, target_lib + ".orig")
        run("ar", "rcs", target_lib, *obj_filenames, cwd=tmpdir)


def remove_symbols_in_file(target_lib, object_file_to_modify, symbols_to_remove):
    """Hide a set of symbols in `object_file_to_modify` by renaming them to
    a unique unused name. pyobjtools doesn't expose an explicit strip
    primitive, but a rename to a never-referenced name is functionally
    equivalent for our use case."""
    with tempfile.TemporaryDirectory() as tmpdir:
        from .tools.pyobjtools import ar as _ar
        _ar.extract_archive(target_lib, tmpdir)

        all_obj_filenames = [fn for fn in os.listdir(tmpdir) if fn.endswith(".o")]
        target_obj_path = os.path.join(tmpdir, object_file_to_modify)

        if not os.path.exists(target_obj_path):
            raise FileNotFoundError(f"Object file {object_file_to_modify} not found in {target_lib}")

        if symbols_to_remove:
            from .tools.pyobjtools import objcopy as _pyobj_objcopy
            rename_map = {sym: f"__mp_stripped_{hashlib.md5(sym.encode()).hexdigest()}" for sym in symbols_to_remove}
            _pyobj_objcopy.rename_obj(target_obj_path, rename_map)

        os.rename(target_lib, target_lib + ".orig")
        run("ar", "rcs", target_lib, *all_obj_filenames, cwd=tmpdir)


def rename_symbols_in_wheel_file(wheel, filename, suffix, protected_symbols = []):
    from wheel.wheelfile import WheelFile
    with TemporaryDirectory() as tmpdir:
        with WheelFile(wheel) as wf:
            wf.extract(filename, tmpdir)
        from __mp__ import rename_symbols_in_file
        rename_symbols_in_file(os.path.join(tmpdir, filename), suffix, protected_symbols)
        with WheelFile(wheel, 'a') as wf:
            wf.write(os.path.join(tmpdir, filename), filename)



# Import the common implementation
from .common import analyze_and_rename_library_symbols
