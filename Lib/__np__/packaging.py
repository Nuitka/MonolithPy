import os
import sys
import platform
import json
import fnmatch

import __np__

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
        raise __np__.NoSuchURL("")
    package_dir_url = getPackageUrl(section, name)
    with __np__.TemporaryDirectory() as temp_dir:
        data_filename = __np__.download_file(
            "{package_dir_url}/index.json".format(**locals()),
            temp_dir,
        )

        with open(data_filename) as data_file:
            return json.loads(data_file.read())


def getBuildScriptName(dir_name, name):
    return "build_script_" + os.path.basename(dir_name).replace("-", "_") + "_" + name

def install_build_tool(name):
    package_index = getPackageJson("build_tools", name)
    package_dir_url = getPackageUrl("build_tools", name)

    if "build_tools" in package_index:
        for tool in package_index["build_tools"]:
            install_build_tool(tool)

    if os.path.isfile(
        os.path.join(__np__.getToolsInstallDir(), name, "version.txt")
    ):
        with open(
            os.path.join(__np__.getToolsInstallDir(), name, "version.txt"), "r"
        ) as f:
            version = f.read()
            if version == package_index["version"]:
                print("Skipping installed build tool {name}.".format(**locals()))
                return

    print("Setting up build tool {name}...".format(**locals()))

    with __np__.TemporaryDirectory() as temp_dir:
        for file in package_index["files"]:
            __np__.download_file(
                "{package_dir_url}/{file}".format(**locals()),
                temp_dir,
            )

        build_script_module_name = getBuildScriptName(temp_dir, name)
        initcwd = os.getcwd()
        initenviron = dict(os.environ)

        build_script_module = __np__.importFileAsModule(
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
        os.path.join(__np__.getToolsInstallDir(), name, "version.txt"), "w"
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
        os.path.join(__np__.getDependencyInstallDir(), name, "version.txt")
    ):
        with open(
            os.path.join(__np__.getDependencyInstallDir(), name, "version.txt"), "r"
        ) as f:
            version = f.read()
            if version == package_index["version"]:
                print("Skipping installed dependency {name}.".format(**locals()))
                return

    print("Compiling dependency {name}...".format(**locals()))

    with __np__.TemporaryDirectory() as temp_dir:
        for file in package_index["files"]:
            __np__.download_file(
                "{package_dir_url}/{file}".format(**locals()),
                temp_dir,
            )

        build_script_module_name = getBuildScriptName(temp_dir, name)
        initcwd = os.getcwd()
        initenviron = dict(os.environ)

        build_script_module = __np__.importFileAsModule(
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
        os.path.join(__np__.getDependencyInstallDir(), name, "version.txt"), "w"
    ) as f:
        f.write(package_index["version"])


def build_package(package_name, version, script_metadata, wheel_directory):
    install_temp_dir = os.path.dirname(os.getcwd())

    if "build_tools" in script_metadata:
        for dep in script_metadata["build_tools"]:
            install_build_tool(dep)

    if "dependencies" in script_metadata:
        for dep in script_metadata["dependencies"]:
            install_dependency(dep)

    for file in script_metadata["files"]:
        package_dir_url = getPackageUrl("packages", package_name)
        __np__.download_file(
            "{package_dir_url}/{file}".format(**locals()),
            install_temp_dir,
        )

    build_script_module_name = getBuildScriptName(install_temp_dir, package_name)

    initcwd = os.getcwd()
    initenviron = dict(os.environ)
    build_script_module = __np__.importFileAsModule(
        build_script_module_name,
        os.path.join(install_temp_dir, script_metadata["build_script"]),
    )

    result = None
    try:
        result = build_script_module.run(wheel_directory)

        assert isinstance(result, str)
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

    return result


def find_source_by_link(package_name, link):
    try:
        package_index = getPackageJson("packages", package_name)
    except __np__.NoSuchURL:
        return None

    if "sources" in package_index:
        for source in package_index["sources"]:
            if source["link"] == link:
                return source

    return None

def get_extra_sources_for_package(package_name):
    try:
        package_index = getPackageJson("packages", package_name)
    except __np__.NoSuchURL:
        return []

    package_sources = []
    if "sources" in package_index:
        from pip._internal.models.candidate import InstallationCandidate
        from pip._internal.models.link import Link

        for source in package_index["sources"]:
            package_sources.append(
                InstallationCandidate(
                    package_name, source["version"], Link(source["link"])
                )
            )
    return package_sources

def find_build_script_for_package(package_name, version=None):
    try:
        package_index = getPackageJson("packages", package_name)
    except __np__.NoSuchURL:
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
