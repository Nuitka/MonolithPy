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
            from __np__.windows import get_short_path
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


def run(*args, **kwargs):
    import subprocess

    stdin = kwargs.pop("stdin", None)
    quiet = kwargs.pop("quiet", False)
    cwd = kwargs.pop("cwd", os.getcwd())
    env = kwargs.pop("env", os.environ.copy())
    assert not kwargs

    # Don't use the pip path customization here. Just replicate our current path.
    env["PYTHONPATH"] = os.pathsep.join([x for x in sys.path if not x.endswith(os.path.sep + "site")])
    path_data = [x for x in env["PATH"].split(os.pathsep) if x != os.path.dirname(sys.executable)]
    env["PATH"] = os.pathsep.join([os.path.dirname(sys.executable)] + path_data)

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

    # Don't use the pip path customization here. Just replicate our current path.
    env["PYTHONPATH"] = os.pathsep.join([x for x in sys.path if not x.endswith(os.path.sep + "site")])
    path_data = [x for x in env["PATH"].split(os.pathsep) if x != os.path.dirname(sys.executable)]
    env["PATH"] = os.pathsep.join([os.path.dirname(sys.executable)] + path_data)

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


def find_build_tool_exe(tool_name, exe):
    if os.name != "nt" and tool_name == "patch":
        return "patch"

    return (
        glob.glob(os.path.join(getToolsInstallDir(), tool_name, exe))
        + glob.glob(os.path.join(getToolsInstallDir(), tool_name, "bin", exe))
    )[0]


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
    return os.path.join(getDependencyInstallDir(), dep_name)


def find_dep_include(dep_name):
    return os.path.join(getDependencyInstallDir(), dep_name, "include")


def find_dep_libs(dep_name):
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
                    f.write(b"#include <np_embed.h>\n" + content)
