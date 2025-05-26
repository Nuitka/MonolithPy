import hashlib
from .common import *


def get_compiler_module():
    __import__("distutils._msvccompiler")
    return sys.modules["distutils._msvccompiler"]


def get_vs_version():
    compiler_module = get_compiler_module()
    from distutils.util import get_host_platform, get_platform
    if hasattr(compiler_module, "PLAT_TO_VCVARS"):
        vcargs = compiler_module.PLAT_TO_VCVARS[get_platform()]
    elif hasattr(compiler_module, "_get_vcvars_spec"):
        vcargs = compiler_module._get_vcvars_spec(get_host_platform(), get_platform())
    else:
        from distutils.compilers.C import msvc
        vcargs = msvc._get_vcvars_spec(get_host_platform(), get_platform())
    vc_env = compiler_module._get_vc_env(vcargs)
    return float(vc_env.get("visualstudioversion"))


def find_compiler_exe(exe):
    compiler_module = get_compiler_module()
    from distutils.util import get_host_platform, get_platform
    if hasattr(compiler_module, "PLAT_TO_VCVARS"):
        vcargs = compiler_module.PLAT_TO_VCVARS[get_platform()]
    elif hasattr(compiler_module, "_get_vcvars_spec"):
        vcargs = compiler_module._get_vcvars_spec(get_host_platform(), get_platform())
    else:
        from distutils.compilers.C import msvc
        vcargs = msvc._get_vcvars_spec(get_host_platform(), get_platform())
        vc_env = compiler_module._get_vc_env(vcargs)
        paths = vc_env.get("path", "").split(os.pathsep)
        return msvc._find_exe(exe, paths)
    vc_env = compiler_module._get_vc_env(vcargs)
    paths = vc_env.get("path", "").split(os.pathsep)
    return compiler_module._find_exe(exe, paths)


def setup_compiler_env():
    compiler_module = get_compiler_module()
    from distutils.util import get_host_platform, get_platform
    if hasattr(compiler_module, "PLAT_TO_VCVARS"):
        vcargs = compiler_module.PLAT_TO_VCVARS[get_platform()]
    elif hasattr(compiler_module, "_get_vcvars_spec"):
        vcargs = compiler_module._get_vcvars_spec(get_host_platform(), get_platform())
    else:
        from distutils.compilers.C import msvc
        vcargs = msvc._get_vcvars_spec(get_host_platform(), get_platform())
    vc_env = compiler_module._get_vc_env(vcargs)
    os.environ.update(vc_env)


def filter_paths_containing(exe_name):
    new_path = []
    for dir in os.environ.get("PATH", "").split(os.pathsep):
        try:
            if not os.path.exists(os.path.join(dir, exe_name)):
                new_path.append(dir)
        except:
            pass

    os.environ["PATH"] = os.pathsep.join(new_path)


def run_compiler_exe(exe, *args):
    return run_with_output(find_compiler_exe(exe), *args)


def msbuild(*args):
    return run_compiler_exe("msbuild.exe", *args)


def nmake(*args):
    return run_compiler_exe("nmake.exe", *args)


def auto_patch_MD_MT_file(fpath):
    try:
        if fpath.endswith("CMakeLists.txt"):
            with open(fpath, "r") as f:
                s = f.read()
            s2 = s.replace("/MD", "/MT")
            s2 = s2.replace("-MD", "-MT")
            s2 = re.sub(
                r"cmake_minimum_required *\( *VERSION [0-9\.]+ *\)",
                """cmake_minimum_required(VERSION 3.15)
            set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
            foreach(flag_var
                        CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
                        CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
                        CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
                        CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
                    if(${flag_var} MATCHES "/MD")
                        string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
                    endif()
                endforeach(flag_var)

         """,
                s2,
                flags=re.IGNORECASE,
            )
            s2 = re.sub(
                r"cmake_policy\(VERSION [0-9\.]+\)", "", s2, flags=re.IGNORECASE
            )
            if s != s2:
                my_print("Fixed up file: %s" % fpath, style="blue")
                with open(fpath, "w") as f:
                    f.write(s2)
        elif not is_file_binary(fpath):
            with open(fpath, "r") as f:
                s = f.read()
            s2 = s.replace("/MD", "/MT")
            s2 = s2.replace("-MD", "-MT")
            if s != s2:
                my_print("Fixed up file: %s" % fpath, style="blue")
                with open(fpath, "w") as f:
                    f.write(s2)
    except Exception:
        pass


def auto_patch_MD_MT(folder):
    for dname, dirs, files in os.walk(folder):
        for fname in files:
            fpath = os.path.join(dname, fname)
            if ".git" in fpath or ".svn" in fpath:
                continue

            auto_patch_MD_MT_file(fpath)


def get_object_symbols(obj):
    try:
        return run_build_tool_exe("clang", "llvm-nm.exe", obj, quiet=True).split("\n")
    except subprocess.CalledProcessError:
        return None


def rename_symbols_in_file(target_lib, prefix, protected_symbols=None):
    if protected_symbols is None:
        protected_symbols = []
    import __np__.packaging
    __np__.packaging.install_build_tool("clang")
    __np__.packaging.install_build_tool("7zip")
    target_lib_abs = os.path.abspath(target_lib)
    with tempfile.TemporaryDirectory() as tmpdir:
        run_build_tool_exe("7zip", "7z.exe", "e", target_lib_abs, "-o" + tmpdir, cwd=tmpdir)
        obj_list = []
        known_symbols = set()
        unmatched_symbols = set()
        keep_symbols = set()
        for obj in os.listdir(tmpdir):
            if obj.endswith(".obj"):
                obj_abs = os.path.abspath(os.path.join(tmpdir, obj))
                obj_list.append(obj_abs)
                symbol_data = get_object_symbols(obj_abs)
                if symbol_data is None:
                    continue
                obj_symbols = [(x[x.rindex(' ') + 1:], x) for x in symbol_data if len(x) > 3]
                obj_symbols = [x for x in obj_symbols if not x[0].startswith(".")]
                for sym in obj_symbols:
                    if any(re.fullmatch(x, sym[0]) for x in protected_symbols):
                        keep_symbols.add(sym[0])
                    if sym[0].startswith("PyInit_") or (not re.fullmatch(r"[a-zA-Z0-9_-]+", sym[0]) and "pybind" not in sym[0]):
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
                run_build_tool_exe("clang", "llvm-objcopy.exe", "--redefine-syms", rename_arg_file, obj, cwd=tmpdir)

        os.rename(target_lib, target_lib + ".orig")
        run_with_output(find_compiler_exe("lib.exe"), "/OUT:" + target_lib, *obj_list)


def rename_init_symbol_in_file(target_lib):
    import __np__.packaging
    __np__.packaging.install_build_tool("clang")
    __np__.packaging.install_build_tool("7zip")
    target_lib_abs = os.path.abspath(target_lib)
    with tempfile.TemporaryDirectory() as tmpdir:
        hasher = hashlib.md5()
        with open(target_lib_abs, "rb") as f_lib:
            for chunk in iter(lambda: f_lib.read(65536), b""):
                hasher.update(chunk)
        file_hash = hasher.hexdigest()

        run_build_tool_exe("7zip", "7z.exe", "e", target_lib_abs, "-o" + tmpdir, cwd=os.getcwd())

        obj_paths_in_tmpdir = []
        modified_any_obj = False

        for item_name in os.listdir(tmpdir):
            if item_name.lower().endswith(".obj"):
                obj_full_path = os.path.join(tmpdir, item_name)
                obj_paths_in_tmpdir.append(obj_full_path)  # Keep track for re-archiving

                symbols_to_rename_map = {}  # old_sym -> new_sym

                symbol_data = get_object_symbols(obj_full_path)
                if symbol_data is None:
                    continue

                for line in symbol_data:
                    line = line.strip()
                    if not line or ' ' not in line: continue
                    try:
                        sym_name = line[line.rindex(' ') + 1:]
                        if not sym_name or sym_name.startswith('.'): continue
                    except ValueError:
                        continue  # Skip lines not matching expected format

                    # Check if it's a PyInit symbol and if it's defined (not 'U')
                    if (sym_name.startswith("PyInit_") or sym_name.startswith("_PyInit_")) and \
                            not (' u ' in line.lower() or line.lower().startswith('u ')):
                        symbols_to_rename_map[sym_name] = f"{sym_name}__np__{file_hash}"

                if not symbols_to_rename_map:
                    continue

                modified_any_obj = True
                with tempfile.TemporaryDirectory() as rename_tmpdir_init:  # Unique temp dir name
                    rename_arg_file_path = os.path.join(rename_tmpdir_init, "rename_init_args.txt")
                    with open(rename_arg_file_path, "w") as f_rename:
                        for old_sym, new_sym in symbols_to_rename_map.items():
                            print(f"Renaming {old_sym} to {new_sym} in {item_name}")
                            f_rename.write(f"{old_sym} {new_sym}\n")

                    run_build_tool_exe("clang", "llvm-objcopy.exe", "--redefine-syms", rename_arg_file_path, obj_full_path, cwd=tmpdir)

        if not modified_any_obj:
            print(f"No PyInit_ symbols found or requiring rename in {target_lib}")
            return

        backup_lib_path = target_lib_abs + ".orig"
        if os.path.exists(backup_lib_path):
            os.remove(backup_lib_path)
        os.rename(target_lib_abs, backup_lib_path)

        run_with_output(find_compiler_exe("lib.exe"), "/OUT:" + target_lib_abs, *obj_paths_in_tmpdir)


def remove_symbols_in_file(target_lib, object_file, symbols):
    target_lib_abs = os.path.abspath(target_lib)
    import __np__.packaging
    __np__.packaging.install_build_tool("clang")
    __np__.packaging.install_build_tool("7zip")
    with tempfile.TemporaryDirectory() as tmpdir:
        run_build_tool_exe("7zip", "7z.exe", "e", target_lib_abs, "-o" + tmpdir, cwd=tmpdir)

        obj_list = [os.path.join(tmpdir, x) for x in os.listdir(tmpdir) if x.endswith(".obj")]

        with tempfile.TemporaryDirectory() as rename_tmpdir:
            remove_arg_file = os.path.join(rename_tmpdir, "remove_args.txt")
            with open(remove_arg_file, "w") as f:
                f.write('\n'.join(symbols) + "\n")

            run_build_tool_exe("clang", "llvm-objcopy.exe", "--strip-symbols", remove_arg_file, os.path.join(tmpdir, object_file), cwd=tmpdir)

        os.rename(target_lib, target_lib + ".orig")
        subprocess.run(["lib", "/OUT:" + target_lib] + obj_list)


def rename_symbols_in_wheel_file(wheel, filename, prefix, protected_symbols = []):
    from wheel.wheelfile import WheelFile
    with TemporaryDirectory() as tmpdir:
        with WheelFile(wheel) as wf:
            wf.extract(filename, tmpdir)
        rename_symbols_in_file(os.path.join(tmpdir, filename), prefix, protected_symbols)
        with WheelFile(wheel, 'a') as wf:
            wf.write(os.path.join(tmpdir, filename), filename)


import os
if os.name == "nt":
    import ctypes
    from ctypes import wintypes
    _GetShortPathNameW = ctypes.windll.kernel32.GetShortPathNameW
    _GetShortPathNameW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
    _GetShortPathNameW.restype = wintypes.DWORD

def get_short_path(path):
    output_buf_size = 0
    while True:
        output_buf = ctypes.create_unicode_buffer(output_buf_size)
        needed = _GetShortPathNameW(path, output_buf, output_buf_size)
        if needed == 0:
            raise ctypes.WinError()
        if output_buf_size >= needed:
            return output_buf.value
        else:
            output_buf_size = needed
