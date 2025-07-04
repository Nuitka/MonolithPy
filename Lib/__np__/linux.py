import hashlib
from .common import *


def get_compiler_module():
    __import__("distutils.unixccompiler")
    return sys.modules["distutils.unixccompiler"]


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

add_link_options({os.path.join(sysconfig.get_config_var('LIBDIR'), 'libnp_embed.a')})
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


def rename_symbols_in_file(target_lib, prefix, protected_symbols=[]):
    target_lib = os.path.abspath(target_lib)
    import __np__.packaging
    __np__.packaging.install_build_tool("clang")
    with tempfile.TemporaryDirectory() as tmpdir:
        run("ar", "-x", target_lib, cwd=tmpdir)
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
    import __np__.packaging
    __np__.packaging.install_build_tool("clang")
    with tempfile.TemporaryDirectory() as tmpdir:
        hasher = hashlib.md5()
        with open(target_lib, "rb") as f:
            for chunk in iter(lambda: f.read(65536), b""):
                hasher.update(chunk)
        file_hash = hasher.hexdigest()

        run("ar", "-x", target_lib, cwd=tmpdir)
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
                        symbols_to_rename_map[sym_name] = f"{sym_name}__np__{file_hash}"

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
        run("ar", "-x", target_lib, cwd=tmpdir)

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
        from __np__ import rename_symbols_in_file
        rename_symbols_in_file(os.path.join(tmpdir, filename), prefix, protected_symbols)
        with WheelFile(wheel, 'a') as wf:
            wf.write(os.path.join(tmpdir, filename), filename)
