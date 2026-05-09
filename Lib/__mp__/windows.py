import hashlib
import json
from .common import *


def get_compiler_module():
    __import__("setuptools._distutils._msvccompiler")
    return sys.modules["setuptools._distutils._msvccompiler"]


def _strip_vc_paths_from_env():
    """vcvarsall.bat prepends VC/SDK paths to %PATH% every time it runs. In
    deeply-nested pip subprocesses this accumulates across levels and
    eventually trips cmd.exe's 8191-char input line limit. Strip the paths
    vcvarsall adds so it can re-add them fresh."""
    vc_markers = (
        "microsoft visual studio",
        "windows kits",
        "microsoft sdks",
        "microsoft.net",
    )
    new_path = [
        p for p in os.environ.get("PATH", "").split(os.pathsep)
        if p and not any(m in p.lower() for m in vc_markers)
    ]
    os.environ["PATH"] = os.pathsep.join(new_path)


def _call_get_vc_env():
    compiler_module = get_compiler_module()
    from setuptools._distutils.util import get_host_platform, get_platform
    if hasattr(compiler_module, "PLAT_TO_VCVARS"):
        vcargs = compiler_module.PLAT_TO_VCVARS[get_platform()]
        vc_env_func = compiler_module._get_vc_env
    elif hasattr(compiler_module, "_get_vcvars_spec"):
        vcargs = compiler_module._get_vcvars_spec(get_host_platform(), get_platform())
        vc_env_func = compiler_module._get_vc_env
    else:
        from setuptools._distutils.compilers.C import msvc
        vcargs = msvc._get_vcvars_spec(get_host_platform(), get_platform())
        vc_env_func = msvc._get_vc_env
    original_path = os.environ.get("PATH", "")
    _strip_vc_paths_from_env()
    try:
        return vc_env_func(vcargs)
    finally:
        os.environ["PATH"] = original_path


def get_vs_version():
    vc_env = _call_get_vc_env()
    return float(vc_env.get("visualstudioversion"))


def find_compiler_exe(exe):
    compiler_module = get_compiler_module()
    vc_env = _call_get_vc_env()
    paths = vc_env.get("path", "").split(os.pathsep)
    find_exe = getattr(compiler_module, "_find_exe", None)
    if find_exe is None:
        from setuptools._distutils.compilers.C import msvc
        find_exe = msvc._find_exe
    return find_exe(exe, paths)


def setup_compiler_env():
    vc_env = _call_get_vc_env()
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


def auto_patch_build_file(fpath):
    try:
        if fpath.endswith("CMakeLists.txt"):
            with open(fpath, "r") as f:
                s = f.read()
            s2 = s.replace("/MD", "/MT")
            s2 = s2.replace("-MD", "-MT")
            escaped_embed_path = os.path.join(sysconfig.get_config_var('base'), 'libs', 'mp_embed.lib').replace("\\", "/")
            escaped_include = sysconfig.get_config_var("INCLUDEPY").replace("\\", "/")
            s2 = re.sub(
                r"cmake_minimum_required *\( *VERSION [0-9\.]+ *\)",
                f"""cmake_minimum_required(VERSION 3.15)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
foreach(flag_var
            CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
            CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
            CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        if(${{flag_var}} MATCHES "/MD")
            string(REGEX REPLACE "/MD" "/MT" ${{flag_var}} "${{${{flag_var}}}}")
        endif()
    endforeach(flag_var)

add_link_options({escaped_embed_path} Shlwapi.lib)
include_directories({escaped_include})
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


def auto_patch_build(folder):
    for dname, dirs, files in os.walk(folder):
        for fname in files:
            fpath = os.path.join(dname, fname)
            if ".git" in fpath or ".svn" in fpath:
                continue

            auto_patch_build_file(fpath)


def get_object_symbols(obj):
    try:
        return run_build_tool_exe("clang", "llvm-nm.exe", obj, quiet=True).split("\n")
    except subprocess.CalledProcessError:
        return None


def is_object_file(filename):
    """Check if a file is an object file for this platform."""
    return filename.endswith(".obj") or filename.endswith(".o")


def is_undefined_symbol(symbol_line):
    """Check if a symbol line from nm represents an undefined symbol."""
    return ' u ' in symbol_line.lower() or symbol_line.lower().startswith('u ')


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
        result = subprocess.run(
            [sys.executable, "-m", "__mp__.tools.extract_ar", lib_path, output_dir],
            capture_output=True,
            text=True,
            check=True
        )
        return True
    except subprocess.CalledProcessError as e:
        my_print(f"Failed to extract {lib_path}: {e.stderr}", style="red")
        return False
    except Exception as e:
        my_print(f"Failed to extract {lib_path}: {e}", style="red")
        return False


def rename_symbols_in_object(obj_path, rename_arg_file, cwd):
    """
    Rename symbols in an object file using a rename arguments file.

    Args:
        obj_path: Path to the object file
        rename_arg_file: Path to the file containing rename arguments
        cwd: Working directory for the operation
    """
    run_build_tool_exe("clang", "llvm-objcopy.exe", "--redefine-syms", rename_arg_file, obj_path, cwd=cwd, quiet=True)


def repack_library(lib_path, obj_list):
    """
    Repack object files into a static library.

    Args:
        lib_path: Path to the output library file
        obj_list: List of object file paths to include
    """
    run_with_output(find_compiler_exe("lib.exe"), "/OUT:" + lib_path, *obj_list, quiet=True)


def rename_symbols_in_file(target_lib, prefix, protected_symbols=None):
    if protected_symbols is None:
        protected_symbols = []
    import __mp__.packaging
    __mp__.packaging.install_build_tool("clang")
    target_lib_abs = os.path.abspath(target_lib)
    with tempfile.TemporaryDirectory() as tmpdir:
        import __mp__.tools.extract_ar
        __mp__.tools.extract_ar.extract_archive(target_lib_abs, tmpdir)
        obj_list = []
        obj_symbols_map = {}
        known_symbols = set()
        unmatched_symbols = set()
        keep_symbols = set()

        # Ignore unreadable objs
        objcopy_exe = find_build_tool_exe("clang", "llvm-objcopy.exe")
        unprocessable_objs = set()
        with tempfile.TemporaryDirectory() as probe_tmpdir:
            empty_syms = os.path.join(probe_tmpdir, "empty.txt")
            with open(empty_syms, "w") as f:
                f.write("")
            for obj in os.listdir(tmpdir):
                if obj.endswith(".obj"):
                    obj_abs = os.path.abspath(os.path.join(tmpdir, obj))
                    obj_list.append(obj_abs)
                    try:
                        run_with_output(objcopy_exe, "--redefine-syms", empty_syms, obj_abs,
                                        cwd=tmpdir, quiet=True)
                    except subprocess.CalledProcessError:
                        sys.stderr.write(f"[rename_symbols] objcopy cannot process {obj}, "
                                         f"protecting its symbols\n")
                        unprocessable_objs.add(obj_abs)

        for obj_abs in obj_list:
            symbol_data = get_object_symbols(obj_abs)
            if symbol_data is None:
                unprocessable_objs.add(obj_abs)
                continue
            obj_syms_parsed = [(x[x.rindex(' ') + 1:], x) for x in symbol_data if len(x) > 3]
            obj_syms_parsed = [x for x in obj_syms_parsed if not x[0].startswith(".")]
            obj_symbols_map[obj_abs] = obj_syms_parsed
            for sym in obj_syms_parsed:
                if any(re.fullmatch(x, sym[0]) for x in protected_symbols):
                    keep_symbols.add(sym[0])
                if sym[0].startswith("PyInit_") or (not re.fullmatch(r"[a-zA-Z0-9_-]+", sym[0]) and "pybind" not in sym[0]):
                    keep_symbols.add(sym[0])
                if ' u ' in sym[1].lower():
                    unmatched_symbols.add(sym[0])
                else:
                    known_symbols.add(sym[0])

        for obj_abs in unprocessable_objs:
            if obj_abs in obj_symbols_map:
                for sym, line in obj_symbols_map[obj_abs]:
                    if ' u ' not in line.lower():
                        keep_symbols.add(sym)

        rename_args = []
        unmatched_symbols = unmatched_symbols - known_symbols
        for sym in known_symbols - unmatched_symbols - keep_symbols:
            rename_args.append(sym + " " + prefix + sym)

        renamable_objs = [obj for obj in obj_list if obj not in unprocessable_objs]
        with tempfile.TemporaryDirectory() as rename_tmpdir:
            rename_arg_file = os.path.join(rename_tmpdir, "rename_args.txt")
            with open(rename_arg_file, "w") as f:
                f.write('\n'.join(rename_args) + "\n")

            for obj in renamable_objs:
                run_build_tool_exe("clang", "llvm-objcopy.exe", "--redefine-syms", rename_arg_file, obj, cwd=tmpdir)

        os.rename(target_lib, target_lib + ".orig")
        run_with_output(find_compiler_exe("lib.exe"), "/OUT:" + target_lib, *obj_list)


def rename_init_symbol_in_file(target_lib):
    import __mp__.packaging
    __mp__.packaging.install_build_tool("clang")
    target_lib_abs = os.path.abspath(target_lib)
    with tempfile.TemporaryDirectory() as tmpdir:
        hasher = hashlib.md5()
        with open(target_lib_abs, "rb") as f_lib:
            for chunk in iter(lambda: f_lib.read(65536), b""):
                hasher.update(chunk)
        file_hash = hasher.hexdigest()

        import __mp__.tools.extract_ar
        __mp__.tools.extract_ar.extract_archive(target_lib_abs, tmpdir)

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
                    if ((sym_name.startswith("PyInit_") or sym_name.startswith("_PyInit_") or
                            "pyx_CommonTypesMetaclass" in sym_name or
                        ("pybind11" in sym_name and '@' not in sym_name and '?' not in sym_name)) and
                            not (' u ' in line.lower() or line.lower().startswith('u '))) or "f2pyinittypes" in sym_name:
                        symbols_to_rename_map[sym_name] = f"{sym_name}__mp__{file_hash}"

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
    import __mp__.packaging
    __mp__.packaging.install_build_tool("clang")
    with tempfile.TemporaryDirectory() as tmpdir:
        import __mp__.tools.extract_ar
        __mp__.tools.extract_ar.extract_archive(target_lib_abs, tmpdir)

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


EXE_MANIFEST = """\
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0" xmlns:asmv3="urn:schemas-microsoft-com:asm.v3">
  <assemblyIdentity type="win32" name="Mini" version="1.0.0.0"/>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <supportedOS Id="{e2011457-1546-43c5-a5fe-008deee3d3f0}"/>
      <supportedOS Id="{35138b9a-5d96-4fbd-8e2d-a2440225f93a}"/>
      <supportedOS Id="{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}"/>
      <supportedOS Id="{1f676c76-80e1-4239-95bb-83d0f6d0da78}"/>
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
    </application>
  </compatibility>
  <asmv3:application>
    <asmv3:windowsSettings xmlns:ws2="http://schemas.microsoft.com/SMI/2016/WindowsSettings">
      <ws2:longPathAware>true</ws2:longPathAware>
    </asmv3:windowsSettings>
  </asmv3:application>
  <dependency>
    <dependentAssembly>
      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>
    </dependentAssembly>
  </dependency>
</assembly>
"""
