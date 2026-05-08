import os
import sys
import platform
import json
import fnmatch

import __mp__

PACKAGE_BASE_URL = os.environ.get(
    "MONOLITHPY_PACKAGE_URL",
    "https://raw.githubusercontent.com/Nuitka/MonolithPy-packages/master",
)


def getPackageUrl(section, name):
    if platform.system() == "Windows":
        if str is bytes:
            section += "/np27-windows"
        else:
            section += "/mp{0}{1}-windows".format(sys.version_info.major, sys.version_info.minor)
    elif platform.system() == "Linux":
        if str is bytes:
            section += "/np27-linux"
        else:
            section += "/mp{0}{1}-linux".format(sys.version_info.major, sys.version_info.minor)
    elif platform.system() == "Darwin":
        if str is bytes:
            section += "/np27-macos"
        else:
            section += "/mp{0}{1}-macos".format(sys.version_info.major, sys.version_info.minor)

    return "{PACKAGE_BASE_URL}/{section}/{name}".format(
        PACKAGE_BASE_URL=PACKAGE_BASE_URL, section=section, name=name.lower()
    )


def getPackageJson(section, name):
    if name is None:
        raise __mp__.NoSuchURL("")
    package_dir_url = getPackageUrl(section, name)
    with __mp__.TemporaryDirectory() as temp_dir:
        data_filename = __mp__.download_file(
            "{package_dir_url}/index.json".format(**locals()),
            temp_dir,
        )

        with open(data_filename) as data_file:
            return json.loads(data_file.read())


def getBuildScriptName(dir_name, name):
    return "build_script_" + os.path.basename(dir_name).replace("-", "_") + "_" + name

def install_build_tool(name):
    import zipfile

    # If the tool is already accessible in any pip build-isolation overlay, no install needed.
    for base_path in __mp__.get_all_pip_base_paths():
        tool_dir = os.path.join(base_path, "build_tools", name)
        if os.path.isdir(tool_dir) and os.listdir(tool_dir):
            return

    package_index = getPackageJson("build_tools", name)
    package_dir_url = getPackageUrl("build_tools", name)

    if "build_tools" in package_index:
        for tool in package_index["build_tools"]:
            install_build_tool(tool)

    if os.path.isfile(
        os.path.join(__mp__.getToolsInstallDir(), name, "version.txt")
    ):
        with open(
            os.path.join(__mp__.getToolsInstallDir(), name, "version.txt"), "r"
        ) as f:
            version = f.read()
            if version == package_index["version"]:
                print("Skipping installed build tool {name}.".format(**locals()))
                return

    print("Setting up build tool {name}...".format(**locals()))

    with __mp__.TemporaryDirectory() as temp_dir:
        for file in package_index["files"]:
            __mp__.download_file(
                "{package_dir_url}/{file}".format(**locals()),
                temp_dir,
            )

        # Download and extract sources so build.py can find them via os.getcwd()
        source_cwd = temp_dir
        if "sources" in package_index:
            extract_base = os.path.join(temp_dir, "sources")
            os.makedirs(extract_base, exist_ok=True)
            for source in package_index["sources"]:
                __mp__.download_extract(source["link"], extract_base)
            # Strip top-level directory (same as pip does for sdists)
            entries = [e for e in os.listdir(extract_base) if not e.startswith(".")]
            if len(entries) == 1 and os.path.isdir(os.path.join(extract_base, entries[0])):
                source_cwd = os.path.join(extract_base, entries[0])
            else:
                source_cwd = extract_base

        wheel_dir = os.path.join(temp_dir, "wheel_output")
        os.makedirs(wheel_dir, exist_ok=True)

        build_script_module_name = getBuildScriptName(temp_dir, name)
        initcwd = os.getcwd()
        initenviron = dict(os.environ)
        os.chdir(source_cwd)

        build_script_module = __mp__.importFileAsModule(
            build_script_module_name,
            os.path.join(temp_dir, package_index["build_script"]),
        )
        try:
            wheel_path = build_script_module.run(wheel_dir)
        finally:
            if build_script_module_name in sys.modules:
                del sys.modules[build_script_module_name]
            try:
                del build_script_module
            except NameError:
                pass
            os.chdir(initcwd)
            os.environ.clear()
            os.environ.update(initenviron)

        # Extract wheel's .data/data/ contents to sys.prefix so tools are usable
        if wheel_path and os.path.isfile(wheel_path):
            import sysconfig as _sysconfig
            sys_prefix = _sysconfig.get_config_var("prefix")
            with zipfile.ZipFile(wheel_path, "r") as zf:
                for member in zf.infolist():
                    parts = member.filename.split("/")
                    if len(parts) >= 3 and parts[0].endswith(".data") and parts[1] == "data":
                        rel_path = "/".join(parts[2:])
                        if not rel_path:
                            continue
                        dest = os.path.join(sys_prefix, rel_path.replace("/", os.sep))
                        if member.filename.endswith("/"):
                            os.makedirs(dest, exist_ok=True)
                        else:
                            os.makedirs(os.path.dirname(dest), exist_ok=True)
                            with zf.open(member) as src, open(dest, "wb") as dst:
                                dst.write(src.read())
                            if os.name != "nt":
                                os.chmod(dest, 0o755)

    with open(
        os.path.join(__mp__.getToolsInstallDir(), name, "version.txt"), "w"
    ) as f:
        f.write(package_index["version"])


def install_dependency(name):
    package_index = getPackageJson("dependencies", name)
    package_dir_url = getPackageUrl("dependencies", name)

    if "build_tools" in package_index:
        for tool in package_index["build_tools"]:
            install_build_tool(tool)
    if "dependencies" in package_index:
        for dep in package_index["dependencies"]:
            install_dependency(dep)

    if os.path.isfile(
        os.path.join(__mp__.getDependencyInstallDir(), name, "version.txt")
    ):
        with open(
            os.path.join(__mp__.getDependencyInstallDir(), name, "version.txt"), "r"
        ) as f:
            version = f.read()
            if version == package_index["version"]:
                print("Skipping installed dependency {name}.".format(**locals()))
                return

    print("Compiling dependency {name}...".format(**locals()))

    with __mp__.TemporaryDirectory() as temp_dir:
        for file in package_index["files"]:
            __mp__.download_file(
                "{package_dir_url}/{file}".format(**locals()),
                temp_dir,
            )

        build_script_module_name = getBuildScriptName(temp_dir, name)
        initcwd = os.getcwd()
        initenviron = dict(os.environ)

        build_script_module = __mp__.importFileAsModule(
            build_script_module_name,
            os.path.join(temp_dir, package_index["build_script"]),
        )
        try:
            build_script_module.run(temp_dir)
        finally:
            if build_script_module_name in sys.modules:
                del sys.modules[build_script_module_name]
            try:
                del build_script_module
            except NameError:
                pass
            os.chdir(initcwd)
            os.environ.clear()
            os.environ.update(initenviron)

    with open(
        os.path.join(__mp__.getDependencyInstallDir(), name, "version.txt"), "w"
    ) as f:
        f.write(package_index["version"])


def build_package(package_name, version, script_metadata, wheel_directory):
    install_temp_dir = os.path.dirname(os.getcwd())

    for file in script_metadata["files"]:
        if package_name.startswith("mpy-dep-"):
            package_dir_url = getPackageUrl("dependencies", package_name[len("mpy-dep-"):])
        elif package_name.startswith("mpy-tool-"):
            package_dir_url = getPackageUrl("build_tools", package_name[len("mpy-tool-"):])
        else:
            package_dir_url = getPackageUrl("packages", package_name)
        __mp__.download_file(
            "{package_dir_url}/{file}".format(**locals()),
            install_temp_dir,
        )

    build_script_module_name = getBuildScriptName(install_temp_dir, package_name)

    initcwd = os.getcwd()
    initenviron = dict(os.environ)
    build_script_module = __mp__.importFileAsModule(
        build_script_module_name,
        os.path.join(install_temp_dir, script_metadata["build_script"]),
    )

    result = None
    try:
        result = build_script_module.run(wheel_directory)

        assert isinstance(result, str)
        __mp__.fix_wheel_metadata(result)
    finally:
        if build_script_module_name in sys.modules:
            del sys.modules[build_script_module_name]
        try:
            del build_script_module
        except NameError:
            pass
        os.chdir(initcwd)
        os.environ.clear()
        os.environ.update(initenviron)

    extra_dependencies = []
    if "dependencies" in script_metadata:
        for dep in script_metadata["dependencies"]:
            extra_dependencies.append("mpy-dep-" + dep)
    __mp__.add_wheel_requirements(result, extra_dependencies)
    __mp__.fix_wheel_metadata(result)

    return result


def find_source_by_link(package_name, link):
    try:
        package_index = getPackageJson("packages", package_name)
    except __mp__.NoSuchURL:
        return None

    if "sources" in package_index:
        for source in package_index["sources"]:
            if source["link"] == link:
                return source

    return None

def make_candidates_from_source_infos(package_name, source_infos):
    from pip._internal.models.candidate import InstallationCandidate
    from pip._internal.models.link import Link

    package_sources = []
    for source in source_infos:
        package_sources.append(
            InstallationCandidate(
                package_name, source["version"], Link(source["link"])
            )
        )

    return package_sources

def get_extra_sources_for_package(package_name):
    try:
        package_index = getPackageJson("packages", package_name)
    except __mp__.NoSuchURL:
        return []

    package_sources = []
    if "sources" in package_index:
        package_sources = make_candidates_from_source_infos(package_name, package_index["sources"])

    return package_sources

def get_sources_for_build_tool(package_name):
    try:
        package_index = getPackageJson("build_tools", package_name)
    except __mp__.NoSuchURL:
        return []

    return make_candidates_from_source_infos(package_name, package_index["sources"])

def get_sources_for_dependency(package_name):
    try:
        package_index = getPackageJson("dependencies", package_name)
    except __mp__.NoSuchURL:
        return []

    return make_candidates_from_source_infos(package_name, package_index["sources"])

def find_build_script_for_package(package_name, version=None):
    if package_name.startswith("mpy-dep-"):
        try:
            return getPackageJson("dependencies", package_name[len("mpy-dep-"):])
        except __mp__.NoSuchURL:
            return None

    if package_name.startswith("mpy-tool-"):
        try:
            return getPackageJson("build_tools", package_name[len("mpy-tool-"):])
        except __mp__.NoSuchURL:
            return None

    try:
        package_index = getPackageJson("packages", package_name)
    except __mp__.NoSuchURL:
        return None

    matched_source = None
    for source in package_index["scripts"]:
        matched_metadata = True
        if version is not None and "metadata" in source and "Version" in source["metadata"]:
            matched_metadata = fnmatch.fnmatch(version, source["metadata"]["Version"])

        if matched_metadata or version is None:
            matched_source = source
            break

    return matched_source
