from __future__ import print_function

import __np__
import ctypes
import distutils
import distutils.ccompiler
import fnmatch
import glob
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import sysconfig
import tempfile
import uuid

MOVEFILE_DELAY_UNTIL_REBOOT = 4

if platform.system() == "Windows":
    interpreter_prefix = sysconfig.get_config_var("base")
else:
    interpreter_prefix = sysconfig.get_config_var("prefix")

def find_files(directory, pattern):
    for root, dirs, files in os.walk(directory):
        for basename in files:
            if fnmatch.fnmatch(basename, pattern):
                filename = os.path.join(root, basename)
                yield filename


def getPythonInitFunctions(compiler, filename):
    if platform.system() == "Windows":
        initFunctions = [
            x.decode("ascii").split(" ")[-1]
            for x in subprocess.check_output(
                [compiler.dumpbin, "/linkermember", filename]
            ).split(b"\r\n")
            if (b"init" if str is bytes else b"PyInit_") in x
        ]
        # MSVC adds an underscore to the beginning of all symbols for x32.
        # We must ignore this underscore.
        if platform.system() == "Windows" and "32" in platform.architecture()[0]:
            initFunctions = [x[1:] if x.startswith("_") else x for x in initFunctions]
    else:
        functions = [
            x.decode("ascii").split(" ")[-1]
            for x in subprocess.check_output(["nm", filename]).split(
                os.linesep.encode("ascii")
            )
        ]
        functions = [x[1:] if x.startswith("_") else x for x in functions]
        initFunctions = [
            x for x in functions if x.startswith("init" if str is bytes else "PyInit_")
        ]

    initFunctions = [
        y for y in initFunctions if "$" not in y and "@" not in y and "?" not in y
    ]

    return initFunctions


def get_lib_hash():
    hash_string = ""
    read_files = set()

    extra_scan_dirs = []
    if platform.system() == "Windows":
        extra_scan_dirs.append(os.path.join(sysconfig.get_config_var('base'), 'libs'))
    else:
        extra_scan_dirs.append(interpreter_prefix)

    # Scan sys.path for any more lingering static libs.
    for path in list(reversed(sys.path)) + extra_scan_dirs:
        # Ignore the working directory so we don't grab duplicate stuff. Also ignore the pip temp path that is
        # injected during a pip install.
        if path == os.getcwd() or "pip-install-" in path or not path:
            continue

        for lib_file in list(glob.glob(os.path.join("**", "*.a"), root_dir=path, recursive=True)) + list(glob.glob(os.path.join("**", "*.lib"), root_dir=path, recursive=True)):
            if os.path.basename(lib_file) in read_files:
                continue

            with open(os.path.join(path, lib_file), 'rb') as f:
                hash_string += hashlib.file_digest(f, "sha512").hexdigest()
            read_files.add(os.path.basename(lib_file))

    return hashlib.sha256(hash_string.encode('ascii')).hexdigest()


def is_lib_valid(path):
    if os.path.isfile(path):
        if "/~" in path or "\\~" in path:
            # Disqualify libs that were removed by pip.
            return False

        with open(path, "rb") as f:
            if f.read(7) == b"!<arch>":
                return True
        return False
    else:
        return True


def run_rebuild():
    try:
        with open(os.path.join(interpreter_prefix, "link.json"), 'r') as f:
            old_link_data = json.load(f)
    except FileNotFoundError:
        old_link_data = {}

    old_hash = old_link_data.get("lib_hash")
    new_hash = get_lib_hash()

    # Try to avoid building if nothing has changed.
    if old_hash == new_hash and __name__ != "__main__":
        print("No native library changes detected. Not rebuilding interpreter.")
        return

    installDir = os.path.dirname(sys.executable)

    has_compiler_vars = sysconfig.get_config_var("CC") and sysconfig.get_config_var("CXX")

    # Make sure we have the same compiler as used originally.
    if has_compiler_vars:
        cc_config_var = sysconfig.get_config_var("CC").split()[0]
        if "CC" in os.environ and os.environ["CC"] != cc_config_var:
            print("Overriding CC variable to Nuitka-Python used '%s' ..." % cc_config_var)
        os.environ["CC"] = cc_config_var

        cxx_config_var = sysconfig.get_config_var("CXX").split()[0]
        if "CXX" in os.environ and os.environ["CXX"] != cxx_config_var:
            print("Overriding CXX variable to Nuitka-Python used '%s' ..." % cxx_config_var)
        os.environ["CXX"] = cxx_config_var

    compiler = distutils.ccompiler.new_compiler(verbose=5)
    if has_compiler_vars:
        compiler.set_executables(
            compiler=cc_config_var,
            compiler_so=cc_config_var,
            linker_exe=cc_config_var,
            compiler_cxx=cxx_config_var,
        )

    try:
        compiler.initialize()
    except AttributeError:
        pass

    foundLibs = {}
    checkedLibs = set()

    from distutils.sysconfig import get_config_var

    ext_suffix = get_config_var("SO" if str is bytes else "EXT_SUFFIX")

    extra_scan_dirs = [__np__.getDependencyInstallDir()]
    if platform.system() == "Windows":
        extra_scan_dirs.append(os.path.join(sysconfig.get_config_var('base'), 'libs'))

    # Scan sys.path for any more lingering static libs.
    for path in list(reversed(sys.path)) + extra_scan_dirs:
        # Ignore the working directory so we don't grab duplicate stuff. Also ignore the pip temp path that is
        # injected during a pip install.
        if path == os.getcwd() or installDir == path or path in installDir or "pip-install-" in path:
            continue
        for file in find_files(
                path, "*.lib" if platform.system() == "Windows" else "*.a"
        ):
            normalized_file = os.path.normpath(file)
            if normalized_file in checkedLibs:
                continue

            filename_base = os.path.basename(file)

            python_lib = sysconfig.get_config_var("LIBRARY")
            if python_lib is not None and filename_base.endswith(sysconfig.get_config_var("LIBRARY")):
                continue

            checkedLibs.add(normalized_file)

            initFunctions = getPythonInitFunctions(compiler, file)
            print(file, initFunctions)

            # If this lib has a Python init function, we should link it in.
            if initFunctions:
                relativePath = os.path.relpath(file, path)
                if "site-packages" in relativePath:
                    continue
                dirpath, filename = os.path.split(relativePath)
                if filename.endswith(".a.a"):
                    filename = filename[:-2]
                if ext_suffix and filename.endswith(ext_suffix):
                    filename = filename[: len(ext_suffix) * -1]
                if filename.endswith(".a"):
                    filename = filename[:-2]
                if filename.endswith(".lib"):
                    filename = filename[:-4]
                if platform.system() != "Windows" and filename.startswith("lib") and len(filename) > 5:
                    filename = filename[3:]
                relative_path = filename
                if dirpath:
                    relative_path = dirpath.replace("\\", ".").replace("/", ".") + "." + relative_path
                if relative_path.startswith("~"):
                    continue
                print(relative_path, file)
                foundLibs[relative_path] = file

    print("Scanning for any additional libs to link...")
    print(foundLibs)

    # Start with the libs needed for a base interpreter.
    if platform.system() == "Windows":
        link_libs = [
            "advapi32",
            "shell32",
            "ole32",
            "oleaut32",
            "kernel32",
            "user32",
            "gdi32",
            "winspool",
            "comdlg32",
            "uuid",
            "odbc32",
            "odbccp32",
            "shlwapi",
            "ws2_32",
            "version",
            "libssl",
            "libcrypto",
            "tcl86ts",
            "tk86ts",
            "Crypt32",
            "Iphlpapi",
            "msi",
            "Rpcrt4",
            "Cabinet",
            "winmm",
            "Netapi32",
            "Bcrypt"
        ]
        if "32" in platform.architecture()[0]:
            link_libs += ["msvcrt"]
    else:
        link_libs = ["m"]

    if platform.system() == "Windows":
        library_dirs = [
            interpreter_prefix,
            os.path.join(interpreter_prefix, "libs"),
            os.path.join(interpreter_prefix, "tcl"),
        ]
    else:
        library_dirs = [
            sysconfig.get_config_var("prefix"),
            sysconfig.get_config_var("LIBDEST"),
            sysconfig.get_config_var("LIBDIR"),
            os.path.join(sysconfig.get_config_var("prefix"), "dependency_libs", "base", "lib"),
        ]

    # Scrape all available libs from the libs directory. We will let the linker worry about filtering out extra symbols.
    for file in find_files(
            sysconfig.get_config_var("prefix"),
            "*.lib" if platform.system() == "Windows" else "*.a",
    ):
        if "interpreter_build" in file:
            continue
        if __np__.getToolsInstallDir() in file:
            continue
        link_libs.append(file)

    for _name, path in foundLibs.items():
        link_libs += [path]

    link_libs = list(set(link_libs))
    library_dirs = list(set(library_dirs))
    extra_link_args = []

    if os.path.isdir(__np__.getToolsInstallDir()):
        for build_tool in os.listdir(__np__.getToolsInstallDir()):
            tool_link_file = os.path.join(__np__.getToolsInstallDir(), build_tool, "link.json")
            if os.path.isfile(tool_link_file):
                with open(tool_link_file, "r") as f:
                    linkData = json.load(f)
                    link_libs += [os.path.join(os.path.dirname(tool_link_file), x) if os.path.isfile(os.path.join(os.path.dirname(tool_link_file), x)) else x for x in linkData.get("libraries", [])]
                    library_dirs += [
                        os.path.join(os.path.dirname(tool_link_file), x)
                        for x in linkData.get("library_dirs", [])
                    ]

    library_dirs += [os.path.join(x, "lib") for x in glob.glob(os.path.join(__np__.getDependencyInstallDir(), "*")) if os.path.isdir(os.path.join(x, "lib"))]

    libIdx = 0
    while libIdx < len(link_libs):
        final_path = None
        lib = link_libs[libIdx]
        if os.path.isfile(lib):
            final_path = lib
        else:
            for dir in library_dirs:
                if os.path.isfile(os.path.join(dir, lib)):
                    final_path = os.path.join(dir, lib)
                    break
                elif os.path.isfile(os.path.join(dir, lib) + ".a"):
                    final_path = os.path.join(dir, lib) + ".a"
                    break
                elif os.path.isfile(os.path.join(dir, "lib" + lib) + ".a"):
                    final_path = os.path.join(dir, "lib" + lib) + ".a"
                    break
                elif os.path.isfile(os.path.join(dir, lib) + ".lib"):
                    final_path = os.path.join(dir, lib) + ".lib"
                    break
        if not final_path:
            libIdx += 1
            continue
        if os.path.isfile(final_path + ".link.json"):
            with open(final_path + ".link.json", "r") as f:
                linkData = json.load(f)
                link_libs += linkData.get("libraries", [])
                library_dirs += [
                    os.path.join(os.path.dirname(final_path), x)
                    for x in linkData.get("library_dirs", [])
                ]
                extra_link_args += linkData.get("extra_postargs", [])
        libIdx += 1

    link_libs = list(set(link_libs))
    library_dirs = list(set(library_dirs))

    print("Generating interpreter sources...")

    staticinitheader = """#ifndef Py_STATICINIT_H
#define Py_STATICINIT_H

#include "object.h"
#include "import.h"

#define NUITKA_PYTHON_STATIC

#if defined(Py_BUILD_CORE) && !defined(Py_BUILD_CORE_MODULE)
#ifdef __cplusplus
extern "C" {
#endif
"""

    inittab_code = ""

    for module_fullname, filename in foundLibs.items():
        if __np__.getToolsInstallDir() in filename:
            continue
        if not is_lib_valid(filename):
            continue

        initFunctions = getPythonInitFunctions(compiler, filename)

        if not initFunctions:
            print("Init not found!", module_fullname, filename)
            continue

        module_basename = module_fullname.split(".")[-1]

        module_initfunc_name = ("init" if str is bytes else "PyInit_") + module_basename

        # We might have packages that rename their build functioons.
        if module_initfunc_name not in initFunctions:
            module_initfunc_name = initFunctions[-1]

        staticinitheader += "   extern  PyObject* " + module_initfunc_name + "(void);\n"
        inittab_code += (
                '   PyImport_AppendInittab("'
                + module_fullname
                + '", '
                + module_initfunc_name
                + ");\n"
        )

    staticinitheader += (
            """
    #ifdef __cplusplus
    }
    #endif // __cplusplus

    static inline void Py_InitStaticModules(void) {
    %s
    }

    #endif

    #endif // !Py_STATICINIT_H
    """
            % inittab_code
    )

    with open(
            os.path.join(sysconfig.get_config_var("INCLUDEPY"), "staticinit.h"), "w"
    ) as f:
        f.write(staticinitheader)

    print("Compiling new interpreter...")

    build_dir = os.path.join(interpreter_prefix, "interpreter_build")

    if os.path.isdir(build_dir):
        shutil.rmtree(build_dir)

    os.makedirs(build_dir)

    include_dirs = [sysconfig.get_config_var("INCLUDEPY")]
    macros = [("Py_BUILD_CORE", None)]

    os.chdir(interpreter_prefix)

    link_flags = []
    compile_flags = []

    if platform.system() == "Windows":
        final_lib_list = []
        for lib in link_libs:
            final_path = lib
            if os.path.isabs(lib):
                final_path = os.path.realpath(lib)
            else:
                for dir in library_dirs:
                    if os.path.isfile(os.path.join(dir, lib)):
                        final_path = os.path.join(dir, lib)
                        break
                    elif os.path.isfile(os.path.join(dir, lib) + ".lib"):
                        final_path = os.path.join(dir, lib) + ".lib"
                        break
                else:
                    # System libraries need to not end in .lib since the linker will automatically append it.
                    if final_path.endswith(".lib"):
                        final_path = final_path[:-4]

                    # There is no such thing as a "limited abi" static lib.
                    if final_path.endswith("python3"):
                        continue
            if final_path not in final_lib_list:
                final_lib_list.append(final_path)

        link_libs = [x for x in final_lib_list if is_lib_valid(x)]

        for file in find_files(
            interpreter_prefix, "*.res"
        ):
            if __np__.getToolsInstallDir() in file:
                continue
            link_flags += [file]

        compiler.compile(
            ["python.c"], output_dir=build_dir, include_dirs=include_dirs, macros=macros
        )

        link_flags = ["/LTCG", "/NODEFAULTLIB:python3.lib", "/FORCE"]
        extra_preargs_ = ["/LTCG", "/NODEFAULTLIB:python3.lib", "/FORCE"]
        #if not ('32bit', 'WindowsPE') == platform.architecture():
        #    # Not Win32 where is no PGO
        #    extra_preargs_.append("/USEPROFILE:PGD=python.pgd")

        compiler.link_executable(
            [os.path.join(build_dir, "python.obj")],
            "python",
            output_dir=build_dir,
            libraries=link_libs,
            library_dirs=library_dirs,
            extra_preargs=extra_preargs_,
        )

        import ctypes.wintypes

        BeginUpdateResource = ctypes.windll.kernel32.BeginUpdateResourceW
        BeginUpdateResource.argtypes = (ctypes.wintypes.LPCWSTR, ctypes.wintypes.BOOL)
        BeginUpdateResource.restype = ctypes.wintypes.HANDLE

        UpdateResourceA = ctypes.windll.kernel32.UpdateResourceA
        UpdateResourceA.argtypes = (
            ctypes.wintypes.HANDLE,
            ctypes.wintypes.LPVOID,
            ctypes.wintypes.LPVOID,
            ctypes.wintypes.WORD,
            ctypes.wintypes.LPVOID,
            ctypes.wintypes.DWORD,
        )

        EndUpdateResource = ctypes.windll.kernel32.EndUpdateResourceA
        EndUpdateResource.argtypes = (ctypes.wintypes.HANDLE, ctypes.wintypes.BOOL)
        EndUpdateResource.restype = ctypes.wintypes.BOOL

        manifest = (
            __np__.EXE_MANIFEST
        ).encode("ascii")

        update_handle = BeginUpdateResource(os.path.join(build_dir, "python.exe"), False)

        RT_MANIFEST = 24
        UpdateResourceA(update_handle, RT_MANIFEST, 1, 1033, manifest, len(manifest))

        EndUpdateResource(update_handle, False)

        if 'Interpreter OK' not in __np__.run_with_output(os.path.join(build_dir, "python.exe"), "-c", "print('Interpreter OK')"):
            raise RuntimeError("Failed to rebuild a working interpreter!")

        # Replace running interpreter by moving current version to a temp file, then marking it for deletion.
        interpreter_path = sys.executable
        tmp = tempfile.NamedTemporaryFile(delete=False)
        tmp.close()
        os.unlink(tmp.name)
        temp_path = tmp.name
        if os.path.splitdrive(temp_path)[0] == os.path.splitdrive(sys.executable)[0]:
            shutil.move(sys.executable, temp_path)
        else:
            shutil.move(sys.executable, sys.executable + uuid.uuid4().hex + ".old_interp")
        ctypes.windll.kernel32.MoveFileExW(temp_path, None, MOVEFILE_DELAY_UNTIL_REBOOT)

        shutil.move(os.path.join(build_dir, "python.exe"), interpreter_path)
    elif platform.system() == "Linux":
        sysconfig_libs = []
        sysconfig_lib_dirs = []
        for arg in (
                ["-lm", "-pthread", "-lutil", "-ldl"]
                + sysconfig.get_config_var("LDFLAGS").split()
                + sysconfig.get_config_var("CFLAGS").split()
                + sysconfig.get_config_var("MODLIBS").split()
                + sysconfig.get_config_var("LIBS").split()
        ):
            if arg.startswith("-l"):
                if arg[2:] not in sysconfig_libs:
                    sysconfig_libs.append(arg[2:])
            elif arg.startswith("-L"):
                if arg[2:] not in sysconfig_lib_dirs:
                    sysconfig_lib_dirs.append(arg[2:])

        link_libs = sysconfig_libs + link_libs
        libpython_lib = [x for x in link_libs if os.path.basename(x).startswith('libpython') and x.endswith(".a")][0]
        link_libs = [libpython_lib] + [x for x in link_libs if x != libpython_lib]
        library_dirs = sysconfig_lib_dirs + library_dirs

        compiler.compile(
            [os.path.join(sysconfig.get_config_var("prefix"), "python.c")],
            output_dir="/",
            include_dirs=include_dirs,
            macros=macros,
        )

        compiler.link_executable(
            objects=[os.path.join(sysconfig.get_config_var("prefix"), "python.o")],
            output_progname="python",
            output_dir=build_dir,
            libraries=link_libs,
            library_dirs=library_dirs,
            extra_preargs=sysconfig.get_config_var("LDFLAGS").split()
                          + [
                              "-l:libsqlite3.a",
                              "-flto",
                              "-fuse-linker-plugin",
                              "-ffat-lto-objects",
                              "-flto-partition=none",
                          ],
        )

        if 'Interpreter OK' not in __np__.run_with_output(os.path.join(build_dir, "python"), "-c", "print('Interpreter OK')"):
            raise RuntimeError("Failed to rebuild a working interpreter!")

        # Replace running interpreter by moving current version to a temp file, then deleting it. This
        # is to avoid Windows locks
        interpreter_path = os.path.realpath(sys.executable)
        tmp = tempfile.NamedTemporaryFile(
            delete=False, dir=os.path.dirname(sys.executable)
        )
        tmp.close()
        os.unlink(tmp.name)
        shutil.move(interpreter_path, tmp.name)
        os.unlink(tmp.name)

        shutil.move(os.path.join(build_dir, "python"), interpreter_path)
    elif platform.system() == "Darwin":
        sysconfig_libs = []
        sysconfig_lib_dirs = []
        for arg in (
                ["-lm", "-pthread", "-lutil", "-ldl", "-lffi"]
                + sysconfig.get_config_var("LDFLAGS").split()
                + sysconfig.get_config_var("CFLAGS").split()
        ):
            if arg.startswith("-l"):
                if arg[2:] not in sysconfig_libs:
                    sysconfig_libs.append(arg[2:])
            elif arg.startswith("-L"):
                if arg[2:] not in sysconfig_lib_dirs:
                    sysconfig_lib_dirs.append(arg[2:])

        link_libs = sysconfig_libs + link_libs
        libpython_lib = [x for x in link_libs if os.path.basename(x).startswith('libpython') and x.endswith(".a")][0]
        link_libs = [libpython_lib] + [x for x in link_libs if x != libpython_lib]
        library_dirs = [x for x in sysconfig_lib_dirs + library_dirs if 'Nuitka-Python-Deps' not in x]

        os.environ["MACOSX_DEPLOYMENT_TARGET"] = "10.9"

        compiler.compile(
            [os.path.join(sysconfig.get_config_var("prefix"), "python.c")],
            output_dir="/",
            include_dirs=include_dirs,
            macros=macros,
        )

        extra_args_combined = [x for x in sysconfig.get_config_var("LDFLAGS").split() if not x.startswith("-L") and not x.startswith("-l")] \
                                + extra_link_args \
                                + [
                                    "-flto=thin",
                                    "-framework", "SystemConfiguration",
                                    "-framework", "CoreFoundation",
                                    "-framework", "Cocoa",
                                    "-framework", "Carbon",
                                    "-framework", "IOKit",
                                    "-framework", "QuartzCore",
                                    "-framework", "CoreServices",
                                    "-framework", "ApplicationServices",
                                    "-framework", "UniformTypeIdentifiers"
                                ]
        num_link_threads = os.environ.get("LINK_THREADS", None)
        if num_link_threads is not None:
            extra_args_combined += ["-Wl,-mllvm,-threads=" + num_link_threads]
        i = 0
        used_frameworks = []
        final_extra_link_args = ["-lstdc++"]
        while i < len(extra_args_combined):
            if extra_args_combined[i].lower() == "-framework":
                if i + 1 < len(extra_args_combined) and \
                        extra_args_combined[i + 1] not in used_frameworks:
                    used_frameworks.append(extra_args_combined[i + 1])
                    final_extra_link_args += ["-framework", extra_args_combined[i + 1]]
                i += 2
            elif extra_args_combined[i].lower() in ("-g", "-xlinker"):
                i += 1
            else:
                final_extra_link_args += [extra_args_combined[i]]
                i += 1

        compiler.link_executable(
            objects=[os.path.join(sysconfig.get_config_var("prefix"), "python.o")],
            output_progname="python",
            output_dir=build_dir,
            libraries=link_libs,
            library_dirs=library_dirs,
            extra_preargs=["-g", "-Xlinker"],
            extra_midargs=final_extra_link_args,
        )

        if 'Interpreter OK' not in __np__.run_with_output(os.path.join(build_dir, "python"), "-c", "print('Interpreter OK')"):
            raise RuntimeError("Failed to rebuild a working interpreter!")

        otool_output = __np__.run_with_output("otool", "-l", os.path.join(build_dir, "python"), quiet=True)
        curr_load_lines = []
        for line in otool_output.split('\n'):
            if line.startswith("Load command") or line.startswith("Section"):
                curr_load_lines.append({})
                continue
            if len(curr_load_lines) == 0:
                continue
            if not line.strip():
                continue

            first_word = [x for x in enumerate(line.split(' ')) if x[1]][0]
            name_len = first_word[0] + len(first_word[1])
            curr_load_lines[-1][line[:name_len].strip()] = line[name_len + 1:]

        for lib in [x for x in curr_load_lines if x.get('name') and not x.get('name').startswith("/") and not x.get('name').startswith("@")]:
            __np__.run_with_output("install_name_tool", "-change", lib['name'].split(' ')[0], os.path.join("@rpath", lib['name'].split(' ')[0]), os.path.join(build_dir, "python"))

        __np__.run_with_output("install_name_tool", "-add_rpath", "@loader_path", os.path.join(build_dir, "python"))

        link_flags = final_extra_link_args

        # Replace running interpreter by moving current version to a temp file, then deleting it. This
        # is to avoid Windows locks
        interpreter_path = os.path.realpath(sys.executable)
        tmp = tempfile.NamedTemporaryFile(
            delete=False, dir=os.path.dirname(sys.executable)
        )
        tmp.close()
        os.unlink(tmp.name)
        shutil.move(interpreter_path, tmp.name)
        os.unlink(tmp.name)

        shutil.move(os.path.join(build_dir, "python"), interpreter_path)

    shutil.rmtree(build_dir, ignore_errors=True)

    with open(os.path.join(interpreter_prefix, "link.json"), "w") as f:
        json.dump(
            {
                "include_dirs": include_dirs,
                "macros": macros,
                "libraries": link_libs,
                "library_dirs": library_dirs,
                "link_flags": link_flags,
                "compile_flags": compile_flags,
                "lib_hash": new_hash
            },
            f,
        )


if __name__ == "__main__":
    run_rebuild()
