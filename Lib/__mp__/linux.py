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
    """Get symbols from an object file using nm."""
    try:
        return run_with_output("nm", obj).split("\n")
    except Exception:
        return None


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
    """
    Extract a static library archive to a directory using subprocess.

    Args:
        lib_path: Path to the library file to extract
        output_dir: Directory to extract to

    Returns:
        True if successful, False otherwise
    """
    try:
        import __mp__.tools.extract_ar
        __mp__.tools.extract_ar.extract_archive(lib_path, output_dir)
        return True
    except Exception as e:
        my_print(f"Failed to extract {lib_path}: {e}", style="red")
        return False


def rename_symbols_in_object(obj_path, rename_arg_file, cwd):
    """
    Rename symbols in an object file using a rename arguments file.
    Handles LLVM bitcode files by converting them first.

    Args:
        obj_path: Path to the object file
        rename_arg_file: Path to the file containing rename arguments
        cwd: Working directory for the operation
    """
    # Check if it's LLVM bitcode and convert if needed
    file_type_output = run_with_output("file", obj_path)
    if "LLVM bitcode" in file_type_output:
        bc_path = obj_path + ".bc"
        os.rename(obj_path, bc_path)
        run("clang", "-c", bc_path, "-o", obj_path)

    # Use objcopy to rename symbols (matching existing rename_symbols_in_file pattern)
    run("objcopy", f"--redefine-syms={rename_arg_file}", obj_path)


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


def rename_symbols_in_file(target_lib, prefix, protected_symbols=[]):
    target_lib = os.path.abspath(target_lib)
    import __mp__.packaging
    __mp__.packaging.install_build_tool("clang")
    with tempfile.TemporaryDirectory() as tmpdir:
        import __mp__.tools.extract_ar
        __mp__.tools.extract_ar.extract_archive(target_lib, tmpdir)
        obj_list = []
        known_symbols = set()
        unmatched_symbols = set()
        keep_symbols = set()

        for obj_filename in os.listdir(tmpdir):
            if obj_filename.endswith(".o"):
                obj_path = os.path.join(tmpdir, obj_filename)
                obj_list.append(obj_path)
                # run_with_output returns string, universal_newlines=True handles text decoding
                symbol_data = run_with_output("nm", obj_path, cwd=tmpdir).split("\n")
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

        rename_args_list = []
        symbols_to_rename = known_symbols - unmatched_symbols - keep_symbols
        for sym in symbols_to_rename:
            rename_args_list.append(f"{sym} {prefix}{sym}")

        if rename_args_list:
            with tempfile.TemporaryDirectory() as rename_tmpdir:
                rename_arg_file = os.path.join(rename_tmpdir, "rename_args.txt")
                with open(rename_arg_file, "w") as f:
                    f.write('\n'.join(rename_args_list) + "\n")

                for obj_p in obj_list:
                    file_type_output = run_with_output("file", obj_p)
                    if "LLVM bitcode" in file_type_output:
                        bc_path = obj_p + ".bc"
                        os.rename(obj_p, bc_path)
                        run("clang", "-c", bc_path, "-o", obj_p)

                    run("objcopy", f"--redefine-syms={rename_arg_file}", obj_p)

        os.rename(target_lib, target_lib + ".orig")
        # Use argument unpacking for the list of object files
        run("ar", "rcs", target_lib, *[os.path.basename(o) for o in obj_list], cwd=tmpdir)


def rename_init_symbol_in_file(target_lib):
    target_lib = os.path.abspath(target_lib)
    import __mp__.packaging
    __mp__.packaging.install_build_tool("clang")
    with tempfile.TemporaryDirectory() as tmpdir:
        hasher = hashlib.md5()
        with open(target_lib, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                hasher.update(chunk)
        file_hash = hasher.hexdigest()

        import __mp__.tools.extract_ar
        __mp__.tools.extract_ar.extract_archive(target_lib, tmpdir)
        obj_list_paths = []
        obj_filenames = []

        for obj_filename in os.listdir(tmpdir):
            if obj_filename.endswith(".o"):
                obj_path = os.path.join(tmpdir, obj_filename)
                obj_list_paths.append(obj_path)
                obj_filenames.append(obj_filename)

                symbols_to_rename_map = {}

                symbol_data = run_with_output("nm", obj_path, cwd=tmpdir).split("\n")
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

                with tempfile.TemporaryDirectory() as rename_tmpdir:
                    rename_arg_file = os.path.join(rename_tmpdir, "rename_args.txt")
                    with open(rename_arg_file, "w") as f:
                        for old_sym, new_sym in symbols_to_rename_map.items():
                            print(f"Renaming {old_sym} to {new_sym}")
                            f.write(f"{old_sym} {new_sym}\n")

                    file_type_output = run_with_output("file", obj_path)
                    if "LLVM bitcode" in file_type_output:
                        bc_path = obj_path + ".bc"
                        os.rename(obj_path, bc_path)
                        run("clang", "-c", bc_path, "-o", obj_path)

                    run("objcopy", f"--redefine-syms={rename_arg_file}", obj_path)

        os.rename(target_lib, target_lib + ".orig")
        run("ar", "rcs", target_lib, *obj_filenames, cwd=tmpdir)


def remove_symbols_in_file(target_lib, object_file_to_modify, symbols_to_remove):
    with tempfile.TemporaryDirectory() as tmpdir:
        import __mp__.tools.extract_ar
        __mp__.tools.extract_ar.extract_archive(target_lib, tmpdir)

        all_obj_filenames = [fn for fn in os.listdir(tmpdir) if fn.endswith(".o")]
        target_obj_path = os.path.join(tmpdir, object_file_to_modify)

        if not os.path.exists(target_obj_path):
            raise FileNotFoundError(f"Object file {object_file_to_modify} not found in {target_lib}")

        if symbols_to_remove:
            with tempfile.TemporaryDirectory() as strip_tmpdir:
                remove_arg_file = os.path.join(strip_tmpdir, "remove_args.txt")
                with open(remove_arg_file, "w") as f:
                    f.write('\n'.join(symbols_to_remove) + "\n")

                run("objcopy", f"--strip-symbols={remove_arg_file}", target_obj_path)

        os.rename(target_lib, target_lib + ".orig")
        run("ar", "rcs", target_lib, *all_obj_filenames, cwd=tmpdir)


def rename_symbols_in_wheel_file(wheel, filename, prefix, protected_symbols = []):
    from wheel.wheelfile import WheelFile
    with TemporaryDirectory() as tmpdir:
        with WheelFile(wheel) as wf:
            wf.extract(filename, tmpdir)
        from __mp__ import rename_symbols_in_file
        rename_symbols_in_file(os.path.join(tmpdir, filename), prefix, protected_symbols)
        with WheelFile(wheel, 'a') as wf:
            wf.write(os.path.join(tmpdir, filename), filename)



# Import the common implementation
from .common import analyze_and_rename_library_symbols
