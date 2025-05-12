from .common import *


def get_compiler_module():
    __import__("distutils.unixccompiler")
    return sys.modules["distutils.unixccompiler"]


def make(*args):
    return run_with_output("make", *args)


def auto_patch_Cython_memcpy(folder):
    # TODO: Have a generic implementation, maybe in nuitka.utils for this
    for dname, dirs, files in os.walk(folder):
        for fname in files:
            fpath = os.path.join(dname, fname)
            if ".git" in fpath or ".svn" in fpath:
                continue

            # TODO: Probably unnecessary
            if fname.endswith(".cc"):
                with open(fpath, "r") as f:
                    s = f.read()
                s2 = s.replace('"-Wl,-wrap,memcpy"', "")

                if s != s2:
                    my_print("Removed Cython config: %s" % fpath, style="blue")
                    with open(fpath, "w") as f:
                        f.write(s2)

            if fname == "setup.py":
                with open(fpath, "r") as f:
                    s = f.read()
                s2 = s.replace("-Wl,-wrap,memcpy", "")

                if s != s2:
                    my_print("Removed memcpy wrapper config: %s" % fpath, style="blue")
                    with open(fpath, "w") as f:
                        f.write(s2)


def rename_symbols_in_file(target_lib, prefix, protected_symbols = []):
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
                    if sym[0].startswith("PyInit_") or sym[0].startswith("_PyInit_") or not re.fullmatch(r"[a-zA-Z0-9_-]+", sym[0]):
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
                try:
                    run_build_tool_exe("clang", "llvm-objcopy", "--redefine-syms", rename_arg_file, obj, cwd=tmpdir)
                except subprocess.CalledProcessError:
                    pass

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


def rename_symbols_in_wheel_file(wheel, filename, prefix, protected_symbols = []):
    from wheel.wheelfile import WheelFile
    with TemporaryDirectory() as tmpdir:
        with WheelFile(wheel) as wf:
            wf.extract(filename, tmpdir)
        rename_symbols_in_file(os.path.join(tmpdir, filename), prefix, protected_symbols)
        with WheelFile(wheel, 'a') as wf:
            wf.write(os.path.join(tmpdir, filename), filename)
