# macOS is mostly the same as linux.
import os.path
from .linux import *

import hashlib


def rename_symbols_in_file(target_lib, prefix, protected_symbols = []):
    target_lib = os.path.abspath(target_lib)
    import __np__.packaging
    __np__.packaging.install_build_tool("clang")
    with tempfile.TemporaryDirectory() as tmpdir:
        subprocess.run(["ar", "-x", target_lib], cwd=tmpdir)
        obj_list = []
        known_symbols = set()
        unmatched_symbols = set()
        keep_symbols = set()
        for obj in os.listdir(tmpdir):
            if obj.endswith(".o"):
                obj_list.append(os.path.join(tmpdir, obj))
                symbol_data = subprocess.check_output(["nm", obj], cwd=tmpdir, text=True).split("\n")
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

        rename_args = []
        unmatched_symbols = unmatched_symbols - known_symbols
        for sym in known_symbols - unmatched_symbols - keep_symbols:
            rename_args.append(sym + " " + prefix + sym)

        with tempfile.TemporaryDirectory() as rename_tmpdir:
            rename_arg_file = os.path.join(rename_tmpdir, "rename_args.txt")
            with open(rename_arg_file, "w") as f:
                f.write('\n'.join(rename_args) + "\n")

            for obj in obj_list:
                obj_path = os.path.join(tmpdir, obj)
                if "LLVM bitcode" in run_with_output("file", obj_path):
                    os.rename(obj_path, obj_path + ".bc")
                    run("clang", "-c", obj_path + ".bc", "-mmacosx-version-min=10.9", "-o", obj_path)

                run_build_tool_exe("clang", "llvm-objcopy", "--redefine-syms", rename_arg_file, obj, cwd=tmpdir)


        os.rename(target_lib, target_lib + ".orig")
        subprocess.run(["libtool", "-static", "-o", target_lib] + obj_list)


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
        subprocess.run(["ar", "-x", target_lib], cwd=tmpdir)
        obj_list = []
        for obj in os.listdir(tmpdir):
            if obj.endswith(".o"):
                obj_path = os.path.join(tmpdir, obj)
                obj_list.append(obj_path)
                symbols_to_rename = set()

                symbol_data = subprocess.check_output(["nm", obj], cwd=tmpdir, text=True).split("\n")
                obj_symbols = [(x[x.rindex(' ') + 1:], x) for x in symbol_data if len(x) > 3]
                obj_symbols = [x for x in obj_symbols if not x[0].startswith(".")]
                for sym in obj_symbols:
                    if sym[0].startswith("PyInit_") or sym[0].startswith("_PyInit_") or \
                            "pyx_CommonTypesMetaclass" in sym[0]:
                        symbols_to_rename.add(sym[0])

                if not symbols_to_rename:
                    continue

                with tempfile.TemporaryDirectory() as rename_tmpdir:
                    rename_arg_file = os.path.join(rename_tmpdir, "rename_args.txt")
                    with open(rename_arg_file, "w") as f:
                        for sym in symbols_to_rename:
                            print(f"Renaming {sym} to {sym}__np__{file_hash}")
                            f.write(f"{sym} {sym}__np__{file_hash}\n")

                    if "LLVM bitcode" in run_with_output("file", obj_path):
                        os.rename(obj_path, obj_path + ".bc")
                        run("clang", "-c", obj_path + ".bc", "-mmacosx-version-min=10.9", "-o", obj_path)
                    run_build_tool_exe("clang", "llvm-objcopy", "--redefine-syms", rename_arg_file, obj_path,
                                        cwd=tmpdir)

        os.rename(target_lib, target_lib + ".orig")
        subprocess.run(["libtool", "-static", "-o", target_lib] + obj_list)


def remove_symbols_in_file(target_lib, object_file, symbols):
    with tempfile.TemporaryDirectory() as tmpdir:
        subprocess.run(["ar", "-x", target_lib], cwd=tmpdir)

        obj_list = [os.path.join(tmpdir, x) for x in os.listdir(tmpdir) if x.endswith(".o")]

        with tempfile.TemporaryDirectory() as rename_tmpdir:
            remove_arg_file = os.path.join(rename_tmpdir, "remove_args.txt")
            with open(remove_arg_file, "w") as f:
                f.write('\n'.join(symbols) + "\n")

            run_build_tool_exe("clang", "llvm-objcopy", "--strip-symbols", remove_arg_file, os.path.join(tmpdir, object_file), cwd=tmpdir)

        os.rename(target_lib, target_lib + ".orig")
        subprocess.run(["libtool", "-static", "-o", target_lib] + obj_list)

