import hashlib
import json
from .common import *


def get_compiler_module():
    __import__("setuptools._distutils._msvccompiler")
    return sys.modules["setuptools._distutils._msvccompiler"]


def get_vs_version():
    compiler_module = get_compiler_module()
    from setuptools._distutils.util import get_host_platform, get_platform
    if hasattr(compiler_module, "PLAT_TO_VCVARS"):
        vcargs = compiler_module.PLAT_TO_VCVARS[get_platform()]
    elif hasattr(compiler_module, "_get_vcvars_spec"):
        vcargs = compiler_module._get_vcvars_spec(get_host_platform(), get_platform())
    else:
        from setuptools._distutils.compilers.C import msvc
        vcargs = msvc._get_vcvars_spec(get_host_platform(), get_platform())
    vc_env = compiler_module._get_vc_env(vcargs)
    return float(vc_env.get("visualstudioversion"))


def find_compiler_exe(exe):
    compiler_module = get_compiler_module()
    from setuptools._distutils.util import get_host_platform, get_platform
    if hasattr(compiler_module, "PLAT_TO_VCVARS"):
        vcargs = compiler_module.PLAT_TO_VCVARS[get_platform()]
    elif hasattr(compiler_module, "_get_vcvars_spec"):
        vcargs = compiler_module._get_vcvars_spec(get_host_platform(), get_platform())
    else:
        from setuptools._distutils.compilers.C import msvc
        vcargs = msvc._get_vcvars_spec(get_host_platform(), get_platform())
        vc_env = compiler_module._get_vc_env(vcargs)
        paths = vc_env.get("path", "").split(os.pathsep)
        return msvc._find_exe(exe, paths)
    vc_env = compiler_module._get_vc_env(vcargs)
    paths = vc_env.get("path", "").split(os.pathsep)
    return compiler_module._find_exe(exe, paths)


def setup_compiler_env():
    compiler_module = get_compiler_module()
    from setuptools._distutils.util import get_host_platform, get_platform
    if hasattr(compiler_module, "PLAT_TO_VCVARS"):
        vcargs = compiler_module.PLAT_TO_VCVARS[get_platform()]
    elif hasattr(compiler_module, "_get_vcvars_spec"):
        vcargs = compiler_module._get_vcvars_spec(get_host_platform(), get_platform())
    else:
        from setuptools._distutils.compilers.C import msvc
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


def auto_patch_build_file(fpath):
    try:
        if fpath.endswith("CMakeLists.txt"):
            with open(fpath, "r") as f:
                s = f.read()
            s2 = s.replace("/MD", "/MT")
            s2 = s2.replace("-MD", "-MT")
            escaped_embed_path = os.path.join(sysconfig.get_config_var('base'), 'libs', 'np_embed.lib').replace("\\", "/")
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


def rename_symbols_in_file(target_lib, prefix, protected_symbols=None):
    if protected_symbols is None:
        protected_symbols = []
    import __np__.packaging
    __np__.packaging.install_build_tool("clang")
    target_lib_abs = os.path.abspath(target_lib)
    with tempfile.TemporaryDirectory() as tmpdir:
        import __np__.tools.extract_ar
        __np__.tools.extract_ar.extract_archive(target_lib_abs, tmpdir)
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
    target_lib_abs = os.path.abspath(target_lib)
    with tempfile.TemporaryDirectory() as tmpdir:
        hasher = hashlib.md5()
        with open(target_lib_abs, "rb") as f_lib:
            for chunk in iter(lambda: f_lib.read(65536), b""):
                hasher.update(chunk)
        file_hash = hasher.hexdigest()

        import __np__.tools.extract_ar
        __np__.tools.extract_ar.extract_archive(target_lib_abs, tmpdir)

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
    with tempfile.TemporaryDirectory() as tmpdir:
        import __np__.tools.extract_ar
        __np__.tools.extract_ar.extract_archive(target_lib_abs, tmpdir)

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


def _extract_archive_subprocess(lib_path, output_dir):
    """
    Extract an archive using __np__.tools.extract_ar in a subprocess.

    Args:
        lib_path: Path to the library file to extract
        output_dir: Directory to extract to

    Returns:
        True if successful, False otherwise
    """
    try:
        result = subprocess.run(
            [sys.executable, "-m", "__np__.tools.extract_ar", lib_path, output_dir],
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


def analyze_and_rename_library_symbols(root_folder, global_suffix, library_pattern="*.lib", protected_symbols=None, symbol_mapping=None, write_debug=False, num_threads=5):
    """
    Analyze all static libraries under a given folder structure and rename symbols.

    Args:
        root_folder: Root directory to search for static libraries
        global_suffix: Global suffix to add to all renamed symbols
        library_pattern: Glob pattern for library files (default: "*.lib")
        protected_symbols: List of regex patterns for symbols that should not be renamed
        symbol_mapping: Optional dict to disambiguate symbols defined in multiple libraries.
                       Format: {
                           "symbol_name": {
                               "use_definition_from": "library_name.lib",
                               "for_libraries": ["lib1.lib", "lib2.lib"]  # optional, if not specified applies to all
                           }
                       }
                       Example: {
                           "d1mach_": {
                               "use_definition_from": "libmach_lib.lib",
                               "for_libraries": ["liblsoda_lib.lib"]
                           }
                       }
        write_debug: Write debug information to a file named rename_data.json in the current directory.
        num_threads: Number of threads to use for parallel processing (default: 5)

    Returns:
        Dictionary with analysis results and renamed libraries
    """
    if protected_symbols is None:
        protected_symbols = []
    if symbol_mapping is None:
        symbol_mapping = {}

    import __np__.packaging
    __np__.packaging.install_build_tool("clang")
    import fnmatch
    from concurrent.futures import ThreadPoolExecutor, as_completed

    library_files = []
    for dirpath, dirnames, filenames in os.walk(root_folder):
        for filename in filenames:
            if fnmatch.fnmatch(filename, library_pattern):
                library_files.append(os.path.join(dirpath, filename))

    if not library_files:
        my_print(f"No library files found in {root_folder}", style="yellow")
        return {}

    my_print(f"Found {len(library_files)} library files", style="blue")

    # Start by extracting all static libraries.
    extraction_tmpdir = tempfile.mkdtemp(prefix="lib_extract_")
    library_symbols = {}
    library_extract_dirs = {}

    try:
        my_print(f"Extracting {len(library_files)} libraries in parallel using {num_threads} threads...", style="blue")

        def extract_library(lib_path):
            """Extract a single library to a unique subdirectory"""
            lib_abs = os.path.abspath(lib_path)
            lib_name = os.path.basename(lib_path)

            # Create unique subdirectory for this library
            hasher = hashlib.md5()
            hasher.update(lib_abs.encode('utf-8'))
            lib_hash = hasher.hexdigest()[:8]
            extract_dir = os.path.join(extraction_tmpdir, f"{lib_name}_{lib_hash}")
            os.makedirs(extract_dir, exist_ok=True)

            # Extract using subprocess to avoid GIL
            if _extract_archive_subprocess(lib_abs, extract_dir):
                return (lib_abs, extract_dir)
            else:
                return (lib_abs, None)

        # Extract all libraries in parallel
        extraction_results = {}
        with ThreadPoolExecutor(max_workers=num_threads) as executor:
            future_to_lib = {executor.submit(extract_library, lib_path): lib_path
                            for lib_path in library_files}

            for future in as_completed(future_to_lib):
                lib_path = future_to_lib[future]
                try:
                    lib_abs, extract_dir = future.result()
                    if extract_dir:
                        extraction_results[lib_abs] = extract_dir
                        my_print(f"  Extracted: {os.path.basename(lib_path)}", style="green")
                    else:
                        my_print(f"  Failed to extract: {os.path.basename(lib_path)}", style="red")
                except Exception as e:
                    my_print(f"  Exception extracting {os.path.basename(lib_path)}: {e}", style="red")

        my_print(f"Successfully extracted {len(extraction_results)} libraries", style="green")

        my_print(f"Analyzing symbols from extracted libraries...", style="blue")

        # For every extracted library,
        for lib_abs, extract_dir in extraction_results.items():
            library_extract_dirs[lib_abs] = extract_dir

            defined_symbols = set()
            undefined_symbols = set()

            # Enumerate all object files...
            for obj in os.listdir(extract_dir):
                if obj.endswith(".obj") or obj.endswith(".o"):
                    obj_abs = os.path.join(extract_dir, obj)
                    symbol_data = get_object_symbols(obj_abs)

                    if symbol_data is None:
                        continue

                    for line in symbol_data:
                        line = line.strip()
                        if len(line) <= 3 or ' ' not in line:
                            continue

                        try:
                            sym_name = line[line.rindex(' ') + 1:]
                            if not sym_name or sym_name.startswith('.'):
                                continue
                        except ValueError:
                            continue

                        if sym_name.startswith("PyInit_") or sym_name.startswith("_PyInit_") or '@' in sym_name or '?' in sym_name:
                            continue

                        # Sort the contained symbols into ones that are defined in this object file and ones that this object file references.
                        if ' u ' in line.lower() or line.lower().startswith('u '):
                            undefined_symbols.add(sym_name)
                        else:
                            defined_symbols.add(sym_name)

            library_symbols[lib_abs] = {
                'defined': defined_symbols,
                'undefined': undefined_symbols
            }

        # Analyze symbol dependencies between libraries
        all_defined = set()
        all_undefined = set()

        for lib_path, symbols in library_symbols.items():
            all_defined.update(symbols['defined'])
            all_undefined.update(symbols['undefined'])

        # First, build a map of which libraries define each symbol
        symbol_definitions = {}  # symbol -> set of lib_paths that define it
        for lib_path, symbols in library_symbols.items():
            for sym in symbols['defined']:
                if sym not in symbol_definitions:
                    symbol_definitions[sym] = set()
                symbol_definitions[sym].add(lib_path)

        # Then, build a map of which libraries reference each symbol
        symbol_references = {}  # symbol -> set of lib_paths that reference it
        for lib_path, symbols in library_symbols.items():
            for sym in symbols['undefined']:
                # Only count as a reference if this library doesn't also define its own version of the symbol
                # (in another object file)
                if sym not in symbols['defined']:
                    if sym not in symbol_references:
                        symbol_references[sym] = set()
                    symbol_references[sym].add(lib_path)

        # External symbols are those that are:
        # - Defined in one or more libraries AND referenced by a library that does NOT define it
        # - OR match protected symbol patterns
        #
        # Key insight: If a symbol is defined in multiple libraries, it's only external if
        # at least one library references it without defining it. If all libraries that
        # reference it also define it, then each library has its own independent implementation
        # and the symbol should be treated as internal to each library.
        external_symbols = set()
        external_symbol_reasons = {}  # symbol -> list of reasons why it's external

        for lib_path, symbols in library_symbols.items():
            lib_name = os.path.basename(lib_path)
            for sym in symbols['defined']:
                reasons = []

                # Check if this symbol is referenced by other libraries (that don't define it)
                if sym in symbol_references:
                    for other_lib_path in symbol_references[sym]:
                        if other_lib_path != lib_path:
                            other_lib_name = os.path.basename(other_lib_path)
                            reasons.append(f"referenced_by:{other_lib_name}")
                            external_symbols.add(sym)

                # Check if symbol matches protected patterns
                for pattern in protected_symbols:
                    if re.fullmatch(pattern, sym):
                        reasons.append(f"protected_pattern:{pattern}")
                        external_symbols.add(sym)
                        break

                # Track reasons for this symbol
                if reasons:
                    if sym not in external_symbol_reasons:
                        external_symbol_reasons[sym] = []
                    external_symbol_reasons[sym].extend([f"{lib_name}:{r}" for r in reasons])

        my_print(f"Found {len(external_symbols)} external symbols", style="green")

        # Check for duplicate definitions of external symbols
        external_symbol_definitions = {}  # symbol -> list of libraries that define it
        for lib_path, symbols in library_symbols.items():
            lib_name = os.path.basename(lib_path)
            for sym in symbols['defined']:
                if sym in external_symbols:
                    if sym not in external_symbol_definitions:
                        external_symbol_definitions[sym] = []
                    external_symbol_definitions[sym].append(lib_name)

        # Collect duplicate external symbols (will filter for unresolved ones later)
        duplicate_externals = {sym: libs for sym, libs in external_symbol_definitions.items() if len(libs) > 1}

        # Check for symbols defined in multiple libraries that are treated as internal
        # (each library has its own implementation and doesn't reference other libraries' versions)
        internal_duplicate_symbols = {}  # symbol -> list of libraries that define it
        for sym, defining_libs in symbol_definitions.items():
            if len(defining_libs) > 1 and sym not in external_symbols:
                internal_duplicate_symbols[sym] = [os.path.basename(lib) for lib in defining_libs]

        if internal_duplicate_symbols:
            my_print(f"\nINFO: Found {len(internal_duplicate_symbols)} symbols with independent implementations in multiple libraries:", style="blue")
            for sym, libs in sorted(list(internal_duplicate_symbols.items())[:10]):  # Show first 10
                my_print(f"  {sym}: independently defined in {len(libs)} libraries: {', '.join(libs[:5])}", style="blue")
            if len(internal_duplicate_symbols) > 10:
                my_print(f"  ... and {len(internal_duplicate_symbols) - 10} more", style="blue")

        # Process symbol_mapping to handle disambiguation of duplicate external symbols
        # This allows users to specify which library's definition should be used for specific symbols
        symbol_disambiguation = {}  # symbol -> {lib_path -> preferred_defining_lib_path}
        applied_mappings = {}  # Track which mappings were applied

        if symbol_mapping:
            my_print(f"\nProcessing {len(symbol_mapping)} symbol disambiguation mappings...", style="blue")

            # Build a reverse map: lib_name -> lib_path
            lib_name_to_path = {os.path.basename(lib_path): lib_path for lib_path in library_symbols.keys()}

            for sym, mapping_info in symbol_mapping.items():
                use_def_from = mapping_info.get("use_definition_from")
                for_libraries = mapping_info.get("for_libraries", None)

                if not use_def_from:
                    my_print(f"  WARNING: Symbol mapping for '{sym}' missing 'use_definition_from'", style="yellow")
                    continue

                # Find the library path for the preferred definition
                preferred_lib_path = None
                for lib_path in library_symbols.keys():
                    if os.path.basename(lib_path) == use_def_from:
                        preferred_lib_path = lib_path
                        break

                if not preferred_lib_path:
                    my_print(f"  WARNING: Library '{use_def_from}' not found for symbol '{sym}'", style="yellow")
                    continue

                # Check if the preferred library actually defines this symbol
                if sym not in library_symbols[preferred_lib_path]['defined']:
                    my_print(f"  WARNING: Symbol '{sym}' not defined in '{use_def_from}'", style="yellow")
                    continue

                # Determine which libraries this mapping applies to
                target_libs = []
                if for_libraries:
                    for lib_name in for_libraries:
                        if lib_name in lib_name_to_path:
                            target_libs.append(lib_name_to_path[lib_name])
                        else:
                            my_print(f"  WARNING: Target library '{lib_name}' not found", style="yellow")
                else:
                    # Apply to all libraries that reference this symbol
                    if sym in symbol_references:
                        target_libs = list(symbol_references[sym])

                if not target_libs:
                    my_print(f"  WARNING: No target libraries found for symbol '{sym}'", style="yellow")
                    continue

                # Store the disambiguation mapping
                if sym not in symbol_disambiguation:
                    symbol_disambiguation[sym] = {}

                for target_lib in target_libs:
                    symbol_disambiguation[sym][target_lib] = preferred_lib_path

                applied_mappings[sym] = {
                    "use_definition_from": use_def_from,
                    "applied_to": [os.path.basename(lib) for lib in target_libs]
                }

                my_print(f"  Mapped '{sym}': use definition from '{use_def_from}' for {len(target_libs)} libraries", style="green")

        library_rename_maps = {}  # lib_path -> {old_sym -> new_sym}
        external_rename_map = {}  # old_sym -> new_sym (for external symbols only)
        disambiguated_symbol_maps = {}  # sym -> {defining_lib_path -> new_name}

        def normalize_sym_part(part):
            return re.sub(r'[^a-zA-Z0-9_]', '_', part)

        # Build library-specific rename maps for internal symbols
        for lib_path, symbols in library_symbols.items():
            lib_name = os.path.splitext(os.path.basename(lib_path))[0]

            # Create hash of library path
            hasher = hashlib.md5()
            hasher.update(lib_path.encode('utf-8'))
            lib_hash = hasher.hexdigest()[:8]

            library_rename_maps[lib_path] = {}

            # Map internal symbols (defined in this lib only)
            internal_symbols = symbols['defined'] - external_symbols
            for sym in internal_symbols:
                new_name = f"{sym}_{global_suffix}_{normalize_sym_part(lib_name)}_{lib_hash}"
                library_rename_maps[lib_path][sym] = new_name

        # Handle disambiguated symbols (external symbols with multiple definitions and user-specified mappings)
        # For these, we rename each definition differently based on which library defines it
        disambiguated_symbols = set()
        for sym in symbol_disambiguation.keys():
            if sym in duplicate_externals:
                disambiguated_symbols.add(sym)
                disambiguated_symbol_maps[sym] = {}

                for defining_lib_path in symbol_definitions[sym]:
                    lib_name = os.path.splitext(os.path.basename(defining_lib_path))[0]
                    hasher = hashlib.md5()
                    hasher.update(defining_lib_path.encode('utf-8'))
                    lib_hash = hasher.hexdigest()[:8]
                    new_name = f"{sym}_{global_suffix}_{normalize_sym_part(lib_name)}_{lib_hash}"
                    disambiguated_symbol_maps[sym][defining_lib_path] = new_name

        # Map external symbols (shared between libraries) - these are the same across all libraries
        # Exclude disambiguated symbols as they are handled separately
        for sym in external_symbols:
            if sym not in disambiguated_symbols:
                new_name = f"{sym}_{global_suffix}"
                external_rename_map[sym] = new_name

        total_mappings = sum(len(m) for m in library_rename_maps.values()) + len(external_rename_map) + sum(len(m) for m in disambiguated_symbol_maps.values())
        my_print(f"Built rename maps with {total_mappings} total symbol mappings", style="green")
        if disambiguated_symbols:
            my_print(f"  Including {len(disambiguated_symbols)} disambiguated symbols with library-specific mappings", style="blue")

        # Report unresolved duplicate external symbols
        # These are duplicates that are NOT resolved by manual disambiguation
        unresolved_duplicates = {sym: libs for sym, libs in duplicate_externals.items()
                                 if sym not in disambiguated_symbols}
        if unresolved_duplicates:
            my_print(f"\nWARNING: Found {len(unresolved_duplicates)} unresolved external symbols defined in multiple libraries:", style="yellow")
            for sym, libs in sorted(unresolved_duplicates.items()):
                my_print(f"  {sym}: defined in {len(libs)} libraries: {', '.join(libs[:5])}", style="yellow")
                if sym in external_symbol_reasons:
                    my_print(f"    Reasons: {', '.join(external_symbol_reasons[sym][:3])}", style="yellow")
            my_print(f"  Note: These symbols will use a shared renamed version across all libraries.", style="yellow")

        debug_data = {
            "summary": {
                "total_libraries": len(library_symbols),
                "total_external_symbols": len(external_symbols),
                "total_internal_symbols": sum(len(m) for m in library_rename_maps.values()),
                "duplicate_external_symbols": len(duplicate_externals),
                "internal_duplicate_symbols": len(internal_duplicate_symbols),
                "disambiguated_symbols": len(disambiguated_symbols)
            },
            "external_symbols": sorted(list(external_symbols)),
            "external_symbol_reasons": external_symbol_reasons,
            "external_symbol_definitions": external_symbol_definitions,
            "duplicate_external_symbols": {
                sym: {
                    "defined_in": libs,
                    "reasons": external_symbol_reasons.get(sym, [])
                }
                for sym, libs in duplicate_externals.items()
            },
            "internal_duplicate_symbols": {
                sym: {
                    "defined_in": libs,
                    "note": "Each library has its own independent implementation"
                }
                for sym, libs in internal_duplicate_symbols.items()
            },
            "disambiguated_symbols": {
                sym: {
                    "defined_in": [os.path.basename(lib) for lib in symbol_definitions[sym]],
                    "mappings": {
                        os.path.basename(lib): disambiguated_symbol_maps[sym][lib]
                        for lib in disambiguated_symbol_maps[sym]
                    },
                    "applied_to": applied_mappings.get(sym, {}).get("applied_to", []),
                    "use_definition_from": applied_mappings.get(sym, {}).get("use_definition_from", "")
                }
                for sym in disambiguated_symbols
            },
            "external_rename_map": external_rename_map,
            "library_internal_counts": {os.path.basename(k): len(v) for k, v in library_rename_maps.items()},
            "library_symbol_details": {
                os.path.basename(lib_path): {
                    "defined_count": len(symbols['defined']),
                    "undefined_count": len(symbols['undefined']),
                    "internal_count": len(library_rename_maps[lib_path]),
                    "external_defined_count": len(symbols['defined'] & external_symbols)
                }
                for lib_path, symbols in library_symbols.items()
            }
        }
        if write_debug:
            with open("rename_data.json", "w") as f:
                json.dump(debug_data, f, indent=2)

        my_print(f"Debug information written to rename_data.json", style="blue")

        # Finally actually rename the symbols.
        def process_library(lib_path):
            """Process a single library - rename symbols using pre-extracted directory and repack"""
            lib_name = os.path.splitext(os.path.basename(lib_path))[0]
            symbols = library_symbols[lib_path]
            lib_rename_map = library_rename_maps[lib_path]

            # Build the complete rename map for this library
            complete_rename_map = {}
            complete_rename_map.update(lib_rename_map)  # Internal symbols for this library
            complete_rename_map.update(external_rename_map)  # External symbols (same for all)

            for sym in disambiguated_symbols:
                # If this library defines the symbol, rename it to its library-specific version
                if sym in symbols['defined']:
                    if lib_path in disambiguated_symbol_maps[sym]:
                        complete_rename_map[sym] = disambiguated_symbol_maps[sym][lib_path]

                # If this library references the symbol, rename it to the preferred definition's version
                elif sym in symbols['undefined']:
                    if sym in symbol_disambiguation and lib_path in symbol_disambiguation[sym]:
                        preferred_lib = symbol_disambiguation[sym][lib_path]
                        if preferred_lib in disambiguated_symbol_maps[sym]:
                            complete_rename_map[sym] = disambiguated_symbol_maps[sym][preferred_lib]

            # Determine which symbols in this library need renaming
            symbols_to_rename = set()

            # Add defined symbols that are in the rename map
            for sym in symbols['defined']:
                if sym in complete_rename_map:
                    symbols_to_rename.add(sym)

            # Add undefined symbols (references) that are in the rename map
            for sym in symbols['undefined']:
                if sym in complete_rename_map:
                    symbols_to_rename.add(sym)

            if not symbols_to_rename:
                my_print(f"  No symbols to rename in {lib_name}", style="yellow")
                return None

            my_print(f"\nProcessing {lib_name}: {len(symbols_to_rename)} symbols to rename", style="blue")

            lib_abs = os.path.abspath(lib_path)
            extract_dir = library_extract_dirs.get(lib_abs)

            if not extract_dir or not os.path.exists(extract_dir):
                my_print(f"  Error: Extract directory not found for {lib_name}", style="red")
                return None

            try:
                obj_list = []

                for obj in os.listdir(extract_dir):
                    if obj.endswith(".obj") or obj.endswith(".o"):
                        obj_abs = os.path.join(extract_dir, obj)
                        obj_list.append(obj_abs)

                rename_args = []
                for sym in symbols_to_rename:
                    if sym in complete_rename_map:
                        rename_args.append(f"{sym} {complete_rename_map[sym]}")

                if rename_args:
                    with tempfile.TemporaryDirectory() as rename_tmpdir:
                        rename_arg_file = os.path.join(rename_tmpdir, "rename_args.txt")
                        with open(rename_arg_file, "w") as f:
                            f.write('\n'.join(rename_args) + "\n")

                        my_print(f"  Renaming {len(rename_args)} symbols in {lib_name}", style="green")

                        for obj in obj_list:
                            try:
                                run_build_tool_exe("clang", "llvm-objcopy.exe", "--redefine-syms",
                                                 rename_arg_file, obj, cwd=extract_dir, quiet=True)
                            except Exception as e:
                                my_print(f"  Warning: Failed to rename symbols in {obj}: {e}", style="yellow")

                    backup_path = lib_abs + ".orig"
                    if os.path.exists(backup_path):
                        os.remove(backup_path)
                    os.rename(lib_abs, backup_path)

                    try:
                        run_with_output(find_compiler_exe("lib.exe"), "/OUT:" + lib_abs, *obj_list, quiet=True)
                        my_print(f"  Successfully renamed symbols in {lib_name}", style="green")

                        defined_renamed = len(symbols['defined'] & symbols_to_rename)
                        undefined_renamed = len(symbols['undefined'] & symbols_to_rename)

                        os.remove(backup_path)

                        return {
                            'lib_path': lib_path,
                            'defined_renamed': defined_renamed,
                            'undefined_renamed': undefined_renamed,
                            'backup': backup_path
                        }
                    except Exception as e:
                        my_print(f"  Error creating new library {lib_name}: {e}", style="red")

                        if os.path.exists(backup_path):
                            os.rename(backup_path, lib_abs)
                        return None
            except Exception as e:
                my_print(f"  Error processing {lib_name}: {e}", style="red")
                return None

        results = {}
        with ThreadPoolExecutor(max_workers=num_threads) as executor:
            future_to_lib = {executor.submit(process_library, lib_path): lib_path
                            for lib_path in library_symbols.keys()}

            for future in as_completed(future_to_lib):
                lib_path = future_to_lib[future]
                try:
                    result = future.result()
                    if result:
                        results[result['lib_path']] = result
                except Exception as e:
                    lib_name = os.path.basename(lib_path)
                    my_print(f"  Exception processing {lib_name}: {e}", style="red")

        my_print(f"\nCompleted processing {len(results)} libraries", style="green")
        return results

    finally:
        # Clean up the extraction temporary directory
        if extraction_tmpdir and os.path.exists(extraction_tmpdir):
            try:
                import shutil
                shutil.rmtree(extraction_tmpdir)
                my_print(f"Cleaned up extraction directory", style="blue")
            except Exception as e:
                my_print(f"Warning: Failed to clean up extraction directory: {e}", style="yellow")


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
