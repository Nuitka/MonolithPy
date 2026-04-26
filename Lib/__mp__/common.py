from __future__ import print_function

import contextlib
import glob
import os
import re
import shutil
import stat
import subprocess
import sys
import sysconfig
import tempfile
from wheel.wheelfile import WheelFile
from email import message_from_bytes
from email.message import Message


def getDependencyInstallDir():
    import sysconfig

    return os.path.join(sysconfig.get_config_var("prefix"), "dependency_libs")


def getToolsInstallDir():
    import sysconfig

    return os.path.join(sysconfig.get_config_var("prefix"), "build_tools")


def getEnableStyleCode(style):
    if style == "pink":
        style = "\033[95m"
    elif style == "blue":
        style = "\033[94m"
    elif style == "green":
        style = "\033[92m"
    elif style == "yellow":
        style = "\033[93m"
    elif style == "red":
        style = "\033[91m"
    elif style == "bold":
        style = "\033[1m"
    elif style == "underline":
        style = "\033[4m"
    else:
        style = None

    return style


_enabled_ansi = False


def _enableAnsi():
    # singleton, pylint: disable=global-statement
    global _enabled_ansi
    if not _enabled_ansi:

        # Only necessary on Windows, as a side effect of this, ANSI colors get enabled
        # for the terminal and never deactivated, so we are free to use them after
        # this.
        if os.name == "nt":
            os.system("")

        _enabled_ansi = True


def getDisableStyleCode():
    return "\033[0m"


def my_print(*args, **kwargs):
    """Make sure we flush after every print.

    Not even the "-u" option does more than that and this is easy enough.

    Use kwarg style=[option] to print in a style listed below
    """

    file_output = kwargs.get("file", sys.stdout)
    is_atty = file_output.isatty()

    if "style" in kwargs:
        style = kwargs["style"]
        del kwargs["style"]

        if "end" in kwargs:
            end = kwargs["end"]
            del kwargs["end"]
        else:
            end = "\n"

        if style is not None and is_atty:
            enable_style = getEnableStyleCode(style)

            if enable_style is None:
                raise ValueError(
                    "%r is an invalid value for keyword argument style" % style
                )

            _enableAnsi()

            print(enable_style, end="", **kwargs)

        print(*args, end=end, **kwargs)

        if style is not None and is_atty:
            print(getDisableStyleCode(), end="", **kwargs)
    else:
        print(*args, **kwargs)

    # Flush the output.
    file_output.flush()


@contextlib.contextmanager
def TemporaryDirectory():

    dirpath = tempfile.mkdtemp()
    yield dirpath

    def delete_readonly_file(_, path, e):
        if len(e) > 2 and e[1].errno == 13:
            os.chmod(path, stat.S_IWRITE)
            os.unlink(path)
        else:
            raise e[1]

    shutil.rmtree(dirpath, onerror=delete_readonly_file)


class NoSuchURL(Exception):
    pass


def copytree(src, dst, symlinks=False, ignore_errors=None, executable=False):
    if not os.path.exists(dst):
        os.makedirs(dst)
    for item in os.listdir(src):
        s = os.path.join(src, item)
        if os.name == "nt":
            from __mp__.windows import get_short_path
            try:
                s = os.path.join(get_short_path(os.path.dirname(s)), os.path.basename(s))
            except:
                pass
        d = os.path.join(dst, item)
        if os.name == "nt":
            try:
                d = os.path.join(get_short_path(os.path.dirname(d)), os.path.basename(d))
            except:
                pass
        if os.path.isdir(s):
            copytree(s, d, symlinks, ignore_errors)
        else:
            try:
                shutil.copy2(s, d)
            except:
                if not ignore_errors:
                    raise
            if executable:
                os.chmod(d, 509)  # 775

def cleanup_file_name(filename):
    if '?' in filename:
        filename = filename[:filename.index('?')]
    return filename

def download_file(url, destination):
    if str is bytes:
        from urllib2 import URLError, HTTPError, Request, urlopen
    else:
        from urllib.request import URLError, HTTPError, Request, urlopen

    try:
        my_print("Attempting to download '%s'." % url, style="blue")

        req = Request(url, headers={"User-Agent": "MonolithPy"})
        with contextlib.closing(urlopen(req)) as fp:
            if (
                "content-disposition" in fp.headers
                and "filename=" in fp.headers["content-disposition"]
            ):
                destination_file = os.path.join(
                    destination,
                    fp.headers["content-disposition"].split("filename=")[-1].strip('"'),
                )
            else:
                destination_file = os.path.join(
                    destination, cleanup_file_name(os.path.basename(fp.geturl()))
                )

            parent_dir = os.path.dirname(destination_file)
            if not os.path.exists(parent_dir):
                os.makedirs(parent_dir)

            with open(destination_file, "wb") as out_file:
                bs = 1024 * 8
                while True:
                    block = fp.read(bs)
                    if not block:
                        break
                    out_file.write(block)

    except HTTPError as e:
        if e.code == 404:
            raise NoSuchURL(url)
        else:
            raise
    except URLError as e:
        # Seems that macOS throws this error instead for file:// links. :(
        if 'Errno 2' in str(e.reason) or 'WinError 3' in str(e.reason):
            raise NoSuchURL(url)
        else:
            raise
    except OSError as e:
        if e.errno == 2:
            raise NoSuchURL(url)
        else:
            raise

    return destination_file


def extract_archive(archive_file, destination=None):
    if destination is None:
        destination = os.path.splitext(archive_file)[0]
        if destination.endswith(".tar"):
            destination = destination[:-4]
    shutil.unpack_archive(archive_file, destination)
    return destination


def download_extract(url, destination):
    with TemporaryDirectory() as dir:
        downloaded_file = download_file(url, dir)
        extract_archive(downloaded_file, destination)


def _get_pip_overlay_script_dirs():
    import tempfile
    dirs = []
    seen = set()
    scripts_name = "Scripts" if os.name == "nt" else "bin"
    try:
        tmpdir = tempfile.gettempdir()
        for entry in os.listdir(tmpdir):
            if entry.startswith("pip-build-env-"):
                for sub in ("overlay", "normal"):
                    scripts = os.path.join(tmpdir, entry, sub, scripts_name)
                    norm = os.path.normcase(scripts)
                    if norm not in seen and os.path.isdir(scripts):
                        seen.add(norm)
                        dirs.append(scripts)
    except Exception:
        pass
    return dirs


def _setup_subprocess_env(env):
    env["PYTHONPATH"] = os.pathsep.join([x for x in sys.path if not x.endswith(os.path.sep + "site")])
    path_data = [x for x in env["PATH"].split(os.pathsep) if x != os.path.dirname(sys.executable)]
    overlay_dirs = _get_pip_overlay_script_dirs()
    env["PATH"] = os.pathsep.join([os.path.dirname(sys.executable)] + overlay_dirs + path_data)


def run(*args, **kwargs):
    import subprocess

    stdin = kwargs.pop("stdin", None)
    quiet = kwargs.pop("quiet", False)
    cwd = kwargs.pop("cwd", os.getcwd())
    env = kwargs.pop("env", os.environ.copy())
    assert not kwargs

    _setup_subprocess_env(env)

    p = subprocess.Popen(
        args,
        universal_newlines=True,
        stdin=stdin,
        env=env,
        cwd=cwd,
    )

    p.wait()
    if p.returncode != 0:
        raise subprocess.CalledProcessError(p.returncode, args)

def run_with_output(*args, **kwargs):
    import subprocess

    stdin = kwargs.pop("stdin", None)
    quiet = kwargs.pop("quiet", False)
    cwd = kwargs.pop("cwd", os.getcwd())
    env = kwargs.pop("env", os.environ.copy())
    assert not kwargs

    _setup_subprocess_env(env)

    p = subprocess.Popen(
        args,
        universal_newlines=True,
        stdin=stdin,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
        cwd=cwd,
    )

    output = ""
    for line in p.stdout:
        if not quiet:
            sys.stdout.write(line)
            sys.stdout.flush()
        output += line
    p.wait()
    if p.returncode != 0:
        raise subprocess.CalledProcessError(p.returncode, args, output)
    return output


def get_wheel_tag():
    import packaging.tags
    return str(next(packaging.tags.sys_tags()))


def get_wheel_name(pkg, version):
    import packaging.utils
    return f"{packaging.utils.canonicalize_name(pkg).replace('-', '_')}-{version}-{get_wheel_tag()}.whl"


def add_wheel_files(wheel_file, target_path, *file_globs, **kwargs):
    """
    Adds files matching glob patterns to the wheel's installation prefix.

    Args:
        wheel_file (wheel.wheelfile.WheelFile): An open WheelFile object.
        target_path (str): The specific folder inside <prefix> where files should go.
                           Example: 'dependency_libs/mylib'
        *file_globs: One or more glob patterns for source files (e.g., 'libs/*.lib').
        base_dir (str, optional): If provided and a matched file path starts with
                                  base_dir, the relative path from base_dir is
                                  preserved in the wheel instead of just the basename.
    """
    base_dir = kwargs.pop("base_dir", None)
    assert not kwargs

    # 1. Derive the .data directory name from the wheel filename
    distribution_namever = wheel_file.parsed_filename.group('namever')
    data_dir_root = f"{distribution_namever}.data/data"

    # 2. Collect all (source_file, arcname) pairs
    file_entries = []

    def _collect_file(source_file, relative_base=None):
        destination_filename = os.path.basename(source_file)
        effective_base = base_dir if relative_base is None else relative_base
        if effective_base is not None and source_file.startswith(effective_base):
            destination_filename = source_file[len(effective_base) + 1:]
        arcname = f"{data_dir_root}/{target_path}/{destination_filename}".replace("\\", "/")
        file_entries.append((source_file, arcname))

    for file_glob in file_globs:
        matched_file_count = 0
        for match in glob.glob(file_glob, recursive=True):
            if os.path.isdir(match):
                effective_base = base_dir if base_dir is not None else os.path.dirname(match)
                for dirpath, dirnames, filenames in os.walk(match):
                    for filename in filenames:
                        full_path = os.path.join(dirpath, filename)
                        _collect_file(full_path, relative_base=effective_base)
                        matched_file_count += 1
            else:
                _collect_file(match)
                matched_file_count += 1

        if matched_file_count == 0:
            my_print(f"WARNING: No files matched glob '{file_glob}'", style="yellow")

    # 3. Use batch write if available and there are enough files to benefit
    if hasattr(wheel_file, 'write_batch') and len(file_entries) > 1:
        wheel_file.write_batch(file_entries)
    else:
        for source_file, arcname in file_entries:
            wheel_file.write(source_file, arcname=arcname)


def add_wheel_dep_include(wheel_file, dependency_name, *file_globs, **kwargs):
    base_dir = kwargs.pop("base_dir", None)
    assert not kwargs

    target_path = f"dependency_libs/{dependency_name}/include"
    add_wheel_files(wheel_file, target_path, *file_globs, base_dir=base_dir)


def add_wheel_dep_libs(wheel_file, dependency_name, *file_globs, **kwargs):
    base_dir = kwargs.pop("base_dir", None)
    assert not kwargs

    target_path = f"dependency_libs/{dependency_name}/lib"
    add_wheel_files(wheel_file, target_path, *file_globs, base_dir=base_dir)


def add_wheel_build_tool(wheel_file, tool_name, *file_globs, **kwargs):
    base_dir = kwargs.pop("base_dir", None)
    assert not kwargs

    target_path = f"build_tools/{tool_name}"
    add_wheel_files(wheel_file, target_path, *file_globs, base_dir=base_dir)


def add_wheel_manifest(wheelfile, package, version):
    # Write the WHEEL file
    wheel_message = Message()
    wheel_message.add_header("Wheel-Version", "1.0")
    wheel_message.add_header("Generator", "wheel mpy")
    wheel_message.add_header("Root-Is-Purelib", "false")
    wheel_message.add_header("Tag", get_wheel_tag())

    wheelfile.writestr(
        f"{wheelfile.dist_info_path}/WHEEL",
        wheel_message.as_string().encode("utf-8"),
    )

    # Write the METADATA file
    wheel_message = Message()
    wheel_message.add_header("Metadata-Version", "2.3")
    wheel_message.add_header("Name", package)
    wheel_message.add_header("Version", version)
    wheel_message.set_payload(f"{package} {version}")

    wheelfile.writestr(
        f"{wheelfile.dist_info_path}/METADATA",
        wheel_message.as_string().encode("utf-8"),
    )


def install_files(dst, *files, **kwargs):
    base_dir = kwargs.pop("base_dir", None)
    executable = kwargs.pop("executable", None)
    ignore_errors = kwargs.pop("ignore_errors", None)
    assert not kwargs

    if not os.path.isdir(dst):
        os.makedirs(dst, exist_ok=True)
    for file_glob in files:
        for file in glob.glob(file_glob):
            destination_filename = os.path.basename(file)
            if base_dir is not None and file.startswith(base_dir):
                destination_filename = file[len(base_dir) + 1 :]
            file_dst = os.path.join(dst, destination_filename)
            if not os.path.exists(os.path.dirname(file_dst)):
                os.makedirs(os.path.dirname(file_dst))
            if os.path.isdir(file):
                copytree(file, os.path.join(dst, destination_filename), executable=executable, ignore_errors=ignore_errors)
            else:
                shutil.copy(file, os.path.join(dst, destination_filename))
                if executable:
                    os.chmod(os.path.join(dst, destination_filename), 509)  # 775


def install_dep_include(dependency_name, *files, **kwargs):
    base_dir = kwargs.pop("base_dir", None)
    assert not kwargs

    dependency_location = os.path.join(
        getDependencyInstallDir(), dependency_name, "include"
    )
    install_files(dependency_location, *files, base_dir=base_dir)


def install_dep_libs(dependency_name, *files, **kwargs):
    base_dir = kwargs.pop("base_dir", None)
    assert not kwargs

    dependency_location = os.path.join(
        getDependencyInstallDir(), dependency_name, "lib"
    )
    install_files(dependency_location, *files, base_dir=base_dir)


def install_build_tool(tool_name, *files, **kwargs):
    base_dir = kwargs.pop("base_dir", None)
    ignore_errors = kwargs.pop("ignore_errors", None)
    assert not kwargs

    dependency_location = os.path.join(getToolsInstallDir(), tool_name)
    install_files(dependency_location, *files, base_dir=base_dir, executable=True, ignore_errors=ignore_errors)


def get_pip_base_path():
    for path in sys.path:
        if os.path.sep + "pip-build-env-" in path and os.path.sep + "overlay" + os.path.sep in path:
            return path[:path.index(os.path.sep + "overlay" + os.path.sep) + 8]
    return None


def get_all_pip_base_paths():
    import tempfile

    seen = set()
    paths = []

    def _add_overlay(overlay_path):
        normalized = os.path.normpath(overlay_path)
        if normalized not in seen and os.path.isdir(normalized):
            seen.add(normalized)
            paths.append(normalized)

    # Check sys.path and PYTHONPATH env var
    candidate_paths = list(sys.path)
    for env_path in os.environ.get("PYTHONPATH", "").split(os.pathsep):
        if env_path:
            candidate_paths.append(env_path)
    for path in candidate_paths:
        normalized = path.replace("/", os.sep)
        sep = os.sep
        marker = sep + "pip-build-env-"
        overlay_marker = sep + "overlay" + sep
        if marker in normalized and overlay_marker in normalized:
            base = normalized[:normalized.index(overlay_marker) + len(overlay_marker) - 1]
            _add_overlay(base)

    # Also scan the system temp dir — outer pip overlays are visible here even if not in sys.path
    try:
        tmpdir = tempfile.gettempdir()
        for entry in os.listdir(tmpdir):
            if entry.startswith("pip-build-env-"):
                overlay = os.path.join(tmpdir, entry, "overlay")
                _add_overlay(overlay)
    except Exception:
        pass

    return paths


def find_build_tool_exe(tool_name, exe):
    if os.name != "nt" and tool_name == "patch":
        return "patch"

    extra_paths = tuple()
    for base_path in get_all_pip_base_paths():
        extra_paths += (os.path.join(base_path, "build_tools", tool_name, exe),
                        os.path.join(base_path, "build_tools", tool_name, "bin", exe))

    extra_paths += (os.path.join(getToolsInstallDir(), tool_name, exe),
                   os.path.join(getToolsInstallDir(), tool_name, "bin", exe))

    for possible_path in extra_paths:
        glob_result = glob.glob(possible_path)
        if glob_result:
            return glob_result[0]

    raise Exception(f"Could not find build tool {tool_name} executable {exe} in {extra_paths}")


def run_build_tool_exe(tool_name, exe, *args, **kwargs):
    return run_with_output(find_build_tool_exe(tool_name, exe), *args, **kwargs)


def apply_patch(patch_file, directory):
    """Apply a patch file to a directory."""
    my_print("Applying patch '%s' to '%s'" % (patch_file, directory))
    with open(patch_file, "rb") as stdin:
        run_build_tool_exe(
            "patch",
            "patch.exe" if os.name == "nt" else "patch",
            "-d",
            directory,
            "-p",
            "1",
            "--verbose",
            stdin=stdin,
        )


def find_dep_root(dep_name):
    base_path = get_pip_base_path()
    if base_path and os.path.isdir(os.path.join(base_path, "dependency_libs", dep_name)):
        return os.path.join(base_path, "dependency_libs", dep_name)
    return os.path.join(getDependencyInstallDir(), dep_name)


def find_dep_include(dep_name):
    base_path = get_pip_base_path()
    if base_path and os.path.isdir(os.path.join(base_path, "dependency_libs", dep_name)):
        return os.path.join(base_path, "dependency_libs", dep_name, "include")
    return os.path.join(getDependencyInstallDir(), dep_name, "include")


def find_dep_libs(dep_name):
    base_path = get_pip_base_path()
    if base_path and os.path.isdir(os.path.join(base_path, "dependency_libs", dep_name)):
        return os.path.join(base_path, "dependency_libs", dep_name, "lib")
    return os.path.join(getDependencyInstallDir(), dep_name, "lib")


def prepend_to_file(file, prepend_str):
    output = prepend_str
    with open(file, "r") as f:
        output += f.read()
    with open(file, "w") as f:
        f.write(output)


def is_file_binary(file_path):
    textchars = bytearray({7, 8, 9, 10, 12, 13, 27} | set(range(0x20, 0x100)) - {0x7F})
    with open(file_path, "rb") as f:
        return bool(f.read(1024).translate(None, textchars))


def get_num_jobs():
    return os.environ.get("NUM_JOBS", os.cpu_count())


def shall_link_statically(name):
    import fnmatch

    static_pattern = os.environ.get("NUITKA_PYTHON_STATIC_PATTERN")
    if not static_pattern or not fnmatch.fnmatch(name, static_pattern):
        return False

    return True


def write_linker_json(
    result_path, libraries, library_dirs, runtime_library_dirs, extra_args
):
    import json

    with open(result_path + ".link.json", "w") as f:
        json.dump(
            {
                "libraries": libraries,
                "library_dirs": library_dirs,
                "runtime_library_dirs": runtime_library_dirs,
                "extra_postargs": extra_args,
            },
            f,
        )

def importFileAsModule(modulename, filename):
    """Import Python module given as a file name.

    Notes:
        Provides a Python version independent way to import any script files.

    Args:
        filename: complete path of a Python script

    Returns:
        Imported Python module with code from the filename.
    """
    import importlib.machinery
    import importlib.util  # pylint: disable=I0021,import-error,no-name-in-module

    build_script_spec = importlib.util.spec_from_loader(
        modulename, importlib.machinery.SourceFileLoader(modulename, filename)
    )
    build_script_module = importlib.util.module_from_spec(build_script_spec)
    build_script_spec.loader.exec_module(build_script_module)
    return build_script_module

def patch_all_source(path):
    for subdir, _, files in os.walk(path):
        for file in files:
            if file.endswith(('.c', '.cxx', '.cpp')):
                filepath = os.path.join(subdir, file)
                with open(filepath, 'rb+') as f:
                    content = f.read()
                    f.seek(0, 0)
                    f.write(b"#include <mp_embed.h>\n" + content)


def analyze_and_rename_library_symbols(root_folder, global_suffix, library_pattern=None, protected_symbol_patterns=None, symbol_mapping=None, write_debug=False, num_threads=5, exclude_libraries=None, exclude_objects=None):
    """
    Analyze all static libraries under a given folder structure and rename symbols.

    Args:
        root_folder: Root directory to search for static libraries
        global_suffix: Global suffix to add to all renamed symbols
        library_pattern: Glob pattern for library files (default: "*.lib" on Windows, "*.a" on Linux/macOS)
        protected_symbol_patterns: List of regex patterns for symbols that should not be renamed
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
        exclude_libraries: List of library names or glob patterns to exclude entirely from symbol renaming.
                          Symbols in excluded libraries will not be renamed, but they will still be analyzed
                          for dependency tracking. Example: ["libfoo.a", "libbar*.a"]
        exclude_objects: Dict mapping library names to lists of object file names or glob patterns to exclude
                        from symbol renaming within that library. Symbols defined in excluded objects will not
                        be renamed, but they will still be analyzed for dependency tracking.
                        Format: {
                            "library_name.a": ["object1.o", "object2*.o"]
                        }
                        Example: {
                            "libcrypto.a": ["x86cpuid.o", "aes*.o"]
                        }

    Returns:
        Dictionary with analysis results and renamed libraries
    """
    # Set default library_pattern based on platform
    if library_pattern is None:
        if sys.platform == "win32":
            library_pattern = "*.lib"
        else:  # Linux and macOS
            library_pattern = "*.a"

    if protected_symbol_patterns is None:
        protected_symbol_patterns = []
    if symbol_mapping is None:
        symbol_mapping = {}
    if exclude_libraries is None:
        exclude_libraries = []
    if exclude_objects is None:
        exclude_objects = {}

    import hashlib
    import json
    import fnmatch
    from concurrent.futures import ThreadPoolExecutor, as_completed

    def is_library_excluded(lib_path):
        """Check if a library should be excluded from symbol renaming."""
        lib_name = os.path.basename(lib_path)
        for pattern in exclude_libraries:
            if fnmatch.fnmatch(lib_name, pattern):
                return True
        return False

    def is_object_excluded(lib_path, obj_name):
        """Check if an object file should be excluded from symbol renaming within a library."""
        lib_name = os.path.basename(lib_path)
        # Check if this library has any object exclusions
        for lib_pattern, obj_patterns in exclude_objects.items():
            if fnmatch.fnmatch(lib_name, lib_pattern):
                for obj_pattern in obj_patterns:
                    if fnmatch.fnmatch(obj_name, obj_pattern):
                        return True
        return False

    # Import platform-specific module
    if sys.platform == "win32":
        from . import windows as platform_module
    elif sys.platform == "darwin":
        from . import darwin as platform_module
    else:
        from . import linux as platform_module

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

            # Extract using platform-specific method
            if platform_module.extract_archive_subprocess(lib_abs, extract_dir):
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

        # Track excluded libraries and objects for reporting
        excluded_library_set = set()
        excluded_object_symbols = {}  # lib_path -> set of symbols from excluded objects

        # For every extracted library,
        for lib_abs, extract_dir in extraction_results.items():
            library_extract_dirs[lib_abs] = extract_dir

            # Check if this library is excluded
            lib_is_excluded = is_library_excluded(lib_abs)
            if lib_is_excluded:
                excluded_library_set.add(lib_abs)
                my_print(f"  Library excluded from renaming: {os.path.basename(lib_abs)}", style="yellow")

            defined_symbols = set()
            undefined_symbols = set()
            local_symbols = set()  # Symbols that are local (not visible outside the object)
            excluded_obj_syms = set()  # Symbols from excluded objects in this library

            # Enumerate all object files...
            for obj in os.listdir(extract_dir):
                if platform_module.is_object_file(obj):
                    obj_abs = os.path.join(extract_dir, obj)
                    symbol_data = platform_module.get_object_symbols(obj_abs)

                    if symbol_data is None:
                        continue

                    # Check if this object is excluded
                    obj_is_excluded = is_object_excluded(lib_abs, obj)
                    if obj_is_excluded:
                        my_print(f"    Object excluded from renaming: {obj} in {os.path.basename(lib_abs)}", style="yellow")

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
                        if platform_module.is_undefined_symbol(line):
                            undefined_symbols.add(sym_name)
                        else:
                            defined_symbols.add(sym_name)
                            # Track if this is a local symbol (can't cause conflicts, shouldn't be renamed)
                            if hasattr(platform_module, 'is_local_symbol') and platform_module.is_local_symbol(line):
                                local_symbols.add(sym_name)
                            # Track symbols from excluded objects
                            if obj_is_excluded:
                                excluded_obj_syms.add(sym_name)

            library_symbols[lib_abs] = {
                'defined': defined_symbols,
                'undefined': undefined_symbols,
                'local': local_symbols
            }
            if excluded_obj_syms:
                excluded_object_symbols[lib_abs] = excluded_obj_syms

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

        # Build a map of which libraries have each symbol as local
        # symbol -> set of lib_paths where it's local
        symbol_local_in = {}
        for lib_path, symbols in library_symbols.items():
            for sym in symbols.get('local', set()):
                if sym not in symbol_local_in:
                    symbol_local_in[sym] = set()
                symbol_local_in[sym].add(lib_path)

        # External symbols are those that are:
        # - Defined GLOBALLY (not local) in one or more libraries AND referenced by a library that does NOT define it
        # - OR match protected symbol patterns
        #
        # Key insight: If a symbol is defined in multiple libraries, it's only external if
        # at least one library references it without defining it. If all libraries that
        # reference it also define it, then each library has its own independent implementation
        # and the symbol should be treated as internal to each library.
        #
        # Additional insight: If a symbol is only referenced by libraries that also define it
        # (i.e., all references are self-references), then it should be treated as internal
        # to each library, even if multiple libraries define it.
        #
        # Self-referential pattern detection: For symbols defined in multiple libraries,
        # check if each defining library only references its own version (self-reference).
        # If a symbol is defined in libraries A and B, and:
        # - Library A references it (but also defines it)
        # - Library B references it (but also defines it)
        # - No other library references it
        # Then this is a self-referential pattern and should be treated as internal.
        #
        # Local symbol handling: If a symbol is local (lowercase in nm output like 't' instead of 'T'),
        # it's not visible outside the object file and can't cause conflicts in that library.
        # But if the same symbol is global in another library, that global version should be renamed.
        # When building per-library rename maps, we skip renaming local symbols in each library.
        external_symbols = set()
        external_symbol_reasons = {}  # symbol -> list of reasons why it's external

        protected_symbols = set()

        for lib_path, symbols in library_symbols.items():
            lib_name = os.path.basename(lib_path)
            local_syms = symbols.get('local', set())
            for sym in symbols['defined']:
                # Skip if this symbol is local in THIS library - but it might be global in another
                # We only skip adding reasons for this library; other libraries with global definitions
                # will still add their reasons
                if sym in local_syms:
                    continue

                reasons = []

                # Check if this symbol is referenced by other libraries (that don't define it)
                if sym in symbol_references:
                    # Only mark as external if at least one referencing library doesn't define it
                    has_external_reference = False
                    for other_lib_path in symbol_references[sym]:
                        # Check if the referencing library also defines this symbol
                        # If it does, this is an internal reference (library using its own version)
                        if sym not in library_symbols[other_lib_path]['defined']:
                            # This library references the symbol but doesn't define it - external reference
                            other_lib_name = os.path.basename(other_lib_path)
                            reasons.append(f"referenced_by:{other_lib_name}")
                            has_external_reference = True

                    if has_external_reference:
                        external_symbols.add(sym)

                # Check if symbol matches protected patterns
                for pattern in protected_symbol_patterns:
                    if re.fullmatch(pattern, sym):
                        reasons.append(f"protected_pattern:{pattern}")
                        external_symbols.add(sym)
                        protected_symbols.add(sym)
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

        # Collect all symbols that should be excluded from renaming
        # This includes symbols from excluded libraries and symbols from excluded objects
        excluded_symbols = set()
        for lib_path in excluded_library_set:
            excluded_symbols.update(library_symbols[lib_path]['defined'])
        for lib_path, obj_syms in excluded_object_symbols.items():
            excluded_symbols.update(obj_syms)

        if excluded_symbols:
            my_print(f"Excluding {len(excluded_symbols)} symbols from renaming (from excluded libraries/objects)", style="blue")

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

            # Skip entirely if this library is excluded
            if lib_path in excluded_library_set:
                continue

            # Get local symbols for this library - these should not be renamed
            local_syms = symbols.get('local', set())

            # Map internal symbols (defined in this lib only)
            # Exclude symbols that are in excluded objects or are local
            internal_symbols = symbols['defined'] - external_symbols
            lib_excluded_obj_syms = excluded_object_symbols.get(lib_path, set())
            for sym in internal_symbols:
                # Skip symbols from excluded objects
                if sym in lib_excluded_obj_syms:
                    continue
                # Skip local symbols - they can't cause conflicts and renaming them breaks things
                if sym in local_syms:
                    continue
                new_name = f"{sym}_{global_suffix}_{normalize_sym_part(lib_name)}_{lib_hash}"
                library_rename_maps[lib_path][sym] = new_name

        # Handle disambiguated symbols (external symbols with multiple definitions and user-specified mappings)
        # For these, we rename each definition differently based on which library defines it
        disambiguated_symbols = set()
        for sym in symbol_disambiguation.keys():
            if sym in duplicate_externals:
                # Skip if this symbol is excluded
                if sym in excluded_symbols:
                    continue
                disambiguated_symbols.add(sym)
                disambiguated_symbol_maps[sym] = {}

                for defining_lib_path in symbol_definitions[sym]:
                    # Skip if this library is excluded
                    if defining_lib_path in excluded_library_set:
                        continue
                    # Skip if this symbol is local in this library - can't be used externally
                    if sym in library_symbols[defining_lib_path].get('local', set()):
                        continue
                    lib_name = os.path.splitext(os.path.basename(defining_lib_path))[0]
                    hasher = hashlib.md5()
                    hasher.update(defining_lib_path.encode('utf-8'))
                    lib_hash = hasher.hexdigest()[:8]
                    new_name = f"{sym}_{global_suffix}_{normalize_sym_part(lib_name)}_{lib_hash}"
                    disambiguated_symbol_maps[sym][defining_lib_path] = new_name

        # Map external symbols (shared between libraries) - these are the same across all libraries
        # Exclude disambiguated symbols as they are handled separately
        # Also exclude symbols from excluded libraries/objects
        for sym in external_symbols:
            if sym not in disambiguated_symbols and sym not in protected_symbols and sym not in excluded_symbols:
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
                "disambiguated_symbols": len(disambiguated_symbols),
                "excluded_libraries": len(excluded_library_set),
                "excluded_symbols": len(excluded_symbols)
            },
            "excluded_libraries": [os.path.basename(lib) for lib in excluded_library_set],
            "excluded_object_symbols": {
                os.path.basename(lib): sorted(list(syms))
                for lib, syms in excluded_object_symbols.items()
            },
            "internal_symbols": {lib: list(syms.keys()) for lib, syms in library_rename_maps.items()},
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
                    "external_defined_count": len(symbols['defined'] & external_symbols),
                    "excluded": lib_path in excluded_library_set
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

            # Check if this library is excluded
            lib_is_excluded = lib_path in excluded_library_set

            symbols = library_symbols[lib_path]
            lib_rename_map = library_rename_maps[lib_path]

            # Build the complete rename map for this library
            complete_rename_map = {}
            # Only include internal symbols if library is not excluded
            if not lib_is_excluded:
                complete_rename_map.update(lib_rename_map)  # Internal symbols for this library
            complete_rename_map.update(external_rename_map)  # External symbols (same for all)

            for sym in disambiguated_symbols:
                # If this library defines the symbol, rename it to its library-specific version
                # But only if the library is not excluded
                if sym in symbols['defined'] and not lib_is_excluded:
                    if lib_path in disambiguated_symbol_maps[sym]:
                        complete_rename_map[sym] = disambiguated_symbol_maps[sym][lib_path]

                # If this library references the symbol, rename it to the preferred definition's version
                # This applies even to excluded libraries - they still need to reference renamed symbols
                elif sym in symbols['undefined']:
                    if sym in symbol_disambiguation and lib_path in symbol_disambiguation[sym]:
                        preferred_lib = symbol_disambiguation[sym][lib_path]
                        if preferred_lib in disambiguated_symbol_maps[sym]:
                            complete_rename_map[sym] = disambiguated_symbol_maps[sym][preferred_lib]

            # Determine which symbols in this library need renaming
            symbols_to_rename = set()

            # Add defined symbols that are in the rename map (only if not excluded)
            if not lib_is_excluded:
                for sym in symbols['defined']:
                    if sym in complete_rename_map:
                        symbols_to_rename.add(sym)

            # Add undefined symbols (references) that are in the rename map
            # This applies even to excluded libraries - they still need to reference renamed symbols
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
                    if platform_module.is_object_file(obj):
                        obj_abs = os.path.join(extract_dir, obj)
                        obj_list.append(obj_abs)

                # Get symbols from excluded objects in this library
                lib_excluded_obj_syms = excluded_object_symbols.get(lib_path, set())

                rename_args = []
                for sym in symbols_to_rename:
                    if sym in complete_rename_map:
                        rename_args.append(f"{sym} {complete_rename_map[sym]}")

                if rename_args:
                    with tempfile.TemporaryDirectory() as rename_tmpdir:
                        rename_arg_file = os.path.join(rename_tmpdir, "rename_args.txt")
                        with open(rename_arg_file, "w") as f:
                            f.write('\n'.join(rename_args) + "\n")

                        # Build a separate rename file for excluded objects that only renames references
                        # (excludes defined symbols from excluded objects)
                        excluded_obj_rename_args = []
                        for sym in symbols_to_rename:
                            if sym in complete_rename_map and sym not in lib_excluded_obj_syms:
                                excluded_obj_rename_args.append(f"{sym} {complete_rename_map[sym]}")

                        excluded_obj_rename_arg_file = None
                        if excluded_obj_rename_args and lib_excluded_obj_syms:
                            excluded_obj_rename_arg_file = os.path.join(rename_tmpdir, "excluded_obj_rename_args.txt")
                            with open(excluded_obj_rename_arg_file, "w") as f:
                                f.write('\n'.join(excluded_obj_rename_args) + "\n")

                        my_print(f"  Renaming {len(rename_args)} symbols in {lib_name}", style="green")

                        for obj in obj_list:
                            obj_name = os.path.basename(obj)
                            obj_is_excluded = is_object_excluded(lib_path, obj_name)

                            try:
                                if obj_is_excluded and excluded_obj_rename_arg_file:
                                    # For excluded objects, use the filtered rename file
                                    # that doesn't rename their defined symbols
                                    platform_module.rename_symbols_in_object(obj, excluded_obj_rename_arg_file, extract_dir)
                                else:
                                    # For normal objects, use the full rename file
                                    platform_module.rename_symbols_in_object(obj, rename_arg_file, extract_dir)
                            except Exception as e:
                                my_print(f"  Warning: Failed to rename symbols in {obj}: {e}", style="yellow")

                    backup_path = lib_abs + ".orig"
                    if os.path.exists(backup_path):
                        os.remove(backup_path)
                    os.rename(lib_abs, backup_path)

                    try:
                        platform_module.repack_library(lib_abs, obj_list)
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
                shutil.rmtree(extraction_tmpdir)
                my_print(f"Cleaned up extraction directory", style="blue")
            except Exception as e:
                my_print(f"Warning: Failed to clean up extraction directory: {e}", style="yellow")


def fix_wheel_metadata(wheel_path):
    """Fix folded RFC 5322 continuation lines in a wheel's own .dist-info/METADATA file."""
    import hashlib, base64, csv, io, zipfile, tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        fixed_path = os.path.join(tmpdir, os.path.basename(wheel_path))
        files_data = {}
        # Only the top-level .dist-info/METADATA (exactly one path component before /METADATA)
        dist_info_metadata_key = None
        with zipfile.ZipFile(wheel_path, 'r') as zin:
            for item in zin.infolist():
                data = zin.read(item.filename)
                parts = item.filename.split('/')
                if len(parts) == 2 and parts[0].endswith('.dist-info') and parts[1] == 'METADATA':
                    dist_info_metadata_key = item.filename
                    text = data.decode('utf-8')
                    text = re.sub(r'\r?\n[ \t]+', ' ', text)
                    text = re.sub(r'^(Requires-Dist: [^;\n]+);[ \t]*$', r'\1', text, flags=re.MULTILINE)
                    data = text.encode('utf-8')
                files_data[item.filename] = (item, data)
        record_key = next((f for f in files_data if f.endswith('/RECORD') or f == 'RECORD'), None)
        if record_key and dist_info_metadata_key:
            record_item, record_data = files_data[record_key]
            rows = list(csv.reader(io.StringIO(record_data.decode('utf-8'))))
            for i, row in enumerate(rows):
                if row and row[0] == dist_info_metadata_key:
                    new_data = files_data[dist_info_metadata_key][1]
                    digest = base64.urlsafe_b64encode(hashlib.sha256(new_data).digest()).rstrip(b'=').decode()
                    rows[i] = [row[0], 'sha256=' + digest, str(len(new_data))]
            buf = io.StringIO()
            csv.writer(buf).writerows(rows)
            files_data[record_key] = (record_item, buf.getvalue().encode('utf-8'))
        with zipfile.ZipFile(fixed_path, 'w', zipfile.ZIP_DEFLATED) as zout:
            for fname, (item, data) in files_data.items():
                zout.writestr(item, data)
        shutil.copy(fixed_path, wheel_path)


def add_wheel_requirements(wheel_path, new_requirements):
    """
    Adds extra requirements to a wheel file, replacing it in-place.
    """

    # Write to a temporary file next to the original, then replace it.
    # Must keep the .whl extension since WheelFile validates the filename.
    base, ext = os.path.splitext(wheel_path)
    tmp_path = base + ".tmp" + ext

    try:
        # 1. Open the source wheel for reading
        with WheelFile(wheel_path, 'r') as wf_in:

            # 2. Open the temporary wheel for writing
            # WheelFile automatically calculates hashes and writes the RECORD file on exit
            with WheelFile(tmp_path, 'w') as wf_out:

                # Identify the METADATA file path inside the wheel
                # It is always inside a directory ending in .dist-info
                metadata_path = None
                for filename in wf_in.namelist():
                    if filename.endswith('.dist-info/METADATA'):
                        metadata_path = filename
                        break

                if not metadata_path:
                    raise ValueError("Invalid Wheel: Could not find METADATA")

                # 3. Iterate through all files in the source wheel
                for zipinfo in wf_in.infolist():

                    # Skip the RECORD file; WheelFile will generate a new one automatically
                    if zipinfo.filename.endswith('/RECORD'):
                        continue

                    # Handle the METADATA file specifically
                    if zipinfo.filename == metadata_path:
                        # Read raw bytes
                        metadata_bytes = wf_in.read(metadata_path)

                        # Parse into a Message object
                        msg: Message = message_from_bytes(metadata_bytes)

                        # Add new Requires-Dist headers
                        # 'add_header' appends duplicates rather than overwriting,
                        # which is exactly what we need for multiple requirements.
                        for req in new_requirements:
                            msg.add_header('Requires-Dist', req)

                        # Write the modified Message back to the new wheel
                        # as_bytes() handles the encoding to utf-8 automatically
                        wf_out.writestr(zipinfo.filename, msg.as_bytes())
                    else:
                        # Copy all other files exactly as they are
                        wf_out.writestr(zipinfo, wf_in.read(zipinfo.filename))

        # 4. Replace the original wheel with the modified one
        os.replace(tmp_path, wheel_path)
    except:
        # Clean up the temporary file on failure
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
        raise
