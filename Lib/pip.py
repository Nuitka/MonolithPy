import fnmatch
import json
import os
import sys
import sysconfig
import warnings
import platform
import rebuildpython
import re
import importlib.machinery

import __mp__
import __mp__.packaging

# Make the standard pip, the real pip module.
# Need to keep a reference alive, or the module will loose all attributes.
if "pip" in sys.modules:
    _this_module = sys.modules["pip"]
    del sys.modules["pip"]

loader = importlib.machinery.SourceFileLoader('pip', os.path.join(os.path.dirname(__file__), "site-packages", "pip", "__init__.py"))
pip = loader.load_module()
sys.modules["pip"] = pip
loader.exec_module(pip)
real_pip_dir = os.path.join(os.path.dirname(__file__), "site-packages")

import ensurepip
builtin_packages = ensurepip._get_packages()


# Use our own pypi proxy at the default to prefer our wheels.
import pip._internal.models.index
pip._internal.models.index.PyPI = pip._internal.models.index.PackageIndex(
    "https://pip-mpy.ohrtech.net/main/", file_storage_domain="assets-mpy.ohrtech.net")


import pip._internal.utils.subprocess

call_subprocess_orig = pip._internal.utils.subprocess.call_subprocess

def our_call_subprocess(
    cmd,
    show_stdout = False,
    cwd = None,
    on_returncode = "raise",
    extra_ok_returncodes = None,
    extra_environ = None,
    unset_environ = None,
    spinner = None,
    log_failed_cmd = True,
    stdout_only = False,
    *,
    command_desc: str,
):
    if extra_ok_returncodes is None:
        our_extra_ok_returncodes = []
    else:
        our_extra_ok_returncodes = list(extra_ok_returncodes)

    # Some packages cause this error code to be returned even if all is ok.
    our_extra_ok_returncodes += [3221225477]
    return call_subprocess_orig(cmd, show_stdout, cwd, on_returncode, our_extra_ok_returncodes, extra_environ, unset_environ, spinner, log_failed_cmd, stdout_only, command_desc=command_desc)

pip._internal.utils.subprocess.call_subprocess = our_call_subprocess


import pip._internal.pyproject

load_pyproject_toml_orig = pip._internal.pyproject.load_pyproject_toml
def our_load_pyproject_toml(pyproject_toml, setup_py, req_name):
    has_pyproject = os.path.isfile(pyproject_toml)
    has_setup = os.path.isfile(setup_py)

    # We will be taking over the build process.
    # mp_script.json is written to build_dir. pyproject.toml may be one level deeper
    # (flat sdist: build_dir/pkg_hash/pyproject.toml) or two levels deeper
    # (sdist with top-level dir: build_dir/pkg_hash/pkg-ver/pyproject.toml).
    _pyproject_dir = os.path.dirname(pyproject_toml)
    _mp_script_path = None
    for _up in ("..", os.path.join("..", "..")):
        _candidate = os.path.join(_pyproject_dir, _up, "mp_script.json")
        if os.path.isfile(_candidate):
            _mp_script_path = os.path.normpath(_candidate)
            break
    if not req_name.startswith("file://") and _mp_script_path is not None:
        with open(_mp_script_path, 'r') as f:
            data = json.load(f)

        package_name = re.split(r'[><=! ]', req_name, maxsplit=1)[0]
        if package_name in data:
            package_data = data[package_name]

            requires = []
            if 'build_requires' in package_data['script_metadata']:
                requires += package_data['script_metadata']['build_requires']
            if 'dist_requires' in package_data['script_metadata']:
                requires += package_data['script_metadata']['dist_requires']
            if 'dependencies' in package_data['script_metadata']:
                requires += [f"mpy-dep-{x}" for x in package_data['script_metadata']['dependencies']]
            if 'build_tools' in package_data['script_metadata']:
                requires += [f"mpy-tool-{x}" for x in package_data['script_metadata']['build_tools']]
            if "mpy-tool-clang" not in requires:
                requires.append("mpy-tool-clang")
            return pip._internal.pyproject.BuildSystemDetails(
                requires, "__mp__.metabuild:managed_build", [], [os.path.dirname(__file__), real_pip_dir])

    result = load_pyproject_toml_orig(pyproject_toml, setup_py, req_name)
    if result is None:
        return None
    return pip._internal.pyproject.BuildSystemDetails(
        [x for x in result.requires if re.split(r'[><=]', x, maxsplit=1)[0] not in builtin_packages] + list(builtin_packages.keys()),
        result.backend, result.check, sys.path + result.backend_path)



pip._internal.pyproject.load_pyproject_toml = our_load_pyproject_toml


import pip._vendor.pyproject_hooks._impl

def norm_and_dont_check(source_tree, requested):
    abs_source = os.path.abspath(source_tree)
    abs_requested = os.path.normpath(os.path.join(abs_source, requested))
    return abs_requested

pip._vendor.pyproject_hooks._impl.norm_and_check = norm_and_dont_check

import pip._internal.distributions.sdist

orig_SourceDistribution_get_build_requires_wheel = pip._internal.distributions.sdist.SourceDistribution._get_build_requires_wheel
def SourceDistribution_get_build_requires_wheel(self):
    our_source = __mp__.packaging.find_source_by_link(self.req.name,self.req.link.url)

    if our_source is not None:
        requires = []
        if 'build_requires' in our_source['script_metadata']:
            requires += our_source['script_metadata']['build_requires']
        if 'dist_requires' in our_source['script_metadata']:
            requires += our_source['script_metadata']['dist_requires']
        if 'dependencies' in our_source['script_metadata']:
            requires += [f"mpy-dep-{x}" for x in our_source['script_metadata']['dependencies']]
        if 'build_tools' in our_source['script_metadata']:
            requires += [f"mpy-tool-{x}" for x in our_source['script_metadata']['build_tools']]
        if "mpy-tool-clang" not in requires:
            requires.append("mpy-tool-clang")
        return requires

    return orig_SourceDistribution_get_build_requires_wheel(self)


pip._internal.distributions.sdist.SourceDistribution._get_build_requires_wheel = SourceDistribution_get_build_requires_wheel


import pip._internal.index.package_finder
from pip._internal import wheel_builder
from pip._internal.models.candidate import InstallationCandidate
from pip._internal.models.link import Link

_PackageFinder = pip._internal.index.package_finder.PackageFinder


class PackageFinder(_PackageFinder):
    def find_all_candidates(self, project_name):
        if project_name in builtin_packages:
            import pathlib

            pkg_version = [x for x in ensurepip._PROJECTS if x[0] == project_name][0][1]

            our_uri = pathlib.Path(os.path.join(os.path.dirname(ensurepip.__file__), '_bundled',
                                                builtin_packages[project_name].wheel_name)).as_uri()
            return [InstallationCandidate(
                project_name, pkg_version, Link(our_uri)
            )]

        if project_name.startswith("mpy-tool-"):
            return __mp__.packaging.get_sources_for_build_tool(project_name[len("mpy-tool-"):])
        if project_name.startswith("mpy-dep-"):
            return __mp__.packaging.get_sources_for_dependency(project_name[len("mpy-dep-"):])

        base_candidates = _PackageFinder.find_all_candidates(self, project_name)

        build_script = __mp__.packaging.find_build_script_for_package(project_name)

        if build_script:
            # If we have a build script, filter out wheels.
            base_candidates = [x for x in base_candidates if not x.link.is_wheel]

        return __mp__.packaging.get_extra_sources_for_package(project_name) + base_candidates


pip._internal.index.package_finder.PackageFinder = PackageFinder

my_path = os.path.abspath(__file__)

def get_runnable_pip() -> str:
    return my_path


import pip._internal.build_env

pip._internal.build_env.get_runnable_pip = get_runnable_pip

import pip._internal.cli.req_command

orig_get_requirements = pip._internal.cli.req_command.RequirementCommand.get_requirements

def our_get_requirements(self, args, options, finder, session):
    reqs = orig_get_requirements(self, args, options, finder, session)
    # This should prevent accidentally updating the pinned bundled packages.
    return [x for x in reqs if x.req is None or x.req.name not in builtin_packages]

pip._internal.cli.req_command.RequirementCommand.get_requirements = our_get_requirements

import pip._internal.req.req_install

orig_install = pip._internal.req.req_install.InstallRequirement.install


def install(
    self,
    root=None,
    home=None,
    prefix=None,
    warn_script_location=True,
    use_user_site=False,
    pycompile=True,
):
    orig_install(self, root, home, prefix, warn_script_location, use_user_site, pycompile)

    rebuildpython.run_rebuild()


pip._internal.req.req_install.InstallRequirement.install = install


import pip._internal.resolution.resolvelib.candidates
import pip._vendor.pkg_resources

orig_prepare_distribution = pip._internal.resolution.resolvelib.candidates.LinkCandidate._prepare_distribution

def _prepare_distribution(self):
    name = self._name
    version = self._version.public if self._version is not None else None

    if name is not None:
        build_script = __mp__.packaging.find_build_script_for_package(name, version)
    else:
        build_script = None

    if build_script is not None:
        data = {}
        try:
            with open(os.path.join(self._factory.preparer.build_dir, 'mp_script.json'), 'r') as f:
                data = json.load(f)
        except FileNotFoundError:
            pass
        data[name] = {
                "name": name,
                "version": version,
                "script_metadata": build_script
            }
        with open(os.path.join(self._factory.preparer.build_dir, 'mp_script.json'), 'w') as f:
            json.dump(data, f)

    result = orig_prepare_distribution(self)
    return result

pip._internal.resolution.resolvelib.candidates.LinkCandidate._prepare_distribution = _prepare_distribution


import pip._vendor.packaging.tags

def our_generic_abi():
    return [pip._vendor.packaging.tags._normalize_string(sysconfig.get_config_var("SOABI"))]

pip._vendor.packaging.tags._generic_abi = our_generic_abi

import pip._vendor.distlib.scripts

if os.name == "nt":
    pip._vendor.distlib.scripts._DEFAULT_MANIFEST = pip._vendor.distlib.scripts.ScriptMaker.manifest = __mp__.EXE_MANIFEST


def main():
    # Work around the error reported in #9540, pending a proper fix.
    # Note: It is essential the warning filter is set *before* importing
    #       pip, as the deprecation happens at import time, not runtime.
    warnings.filterwarnings(
        "ignore", category=DeprecationWarning, module=".*packaging\\.version"
    )

    if sysconfig.get_config_var("CC"):
        cc_config_var = sysconfig.get_config_var("CC").split()[0]
        if "CC" in os.environ and os.environ["CC"] != cc_config_var:
            print("Overriding CC variable to MonolithPy used '%s' ..." % cc_config_var)
        os.environ["CC"] = cc_config_var

    if sysconfig.get_config_var("CXX"):
        cxx_config_var = sysconfig.get_config_var("CXX").split()[0]
        if "CXX" in os.environ and os.environ["CXX"] != cxx_config_var:
            print("Overriding CXX variable to MonolithPy used '%s' ..." % cxx_config_var)
        os.environ["CXX"] = cxx_config_var

    if platform.system() == "Darwin":
        os.environ["MACOSX_DEPLOYMENT_TARGET"] = "10.9"

    # pkg-config uses absolute paths, which do not allow for
    if platform.system() != "Windows":
        os.environ["PKG_CONFIG"] = "/disabled"

    import site

    for path in site.getsitepackages():
        # Note: Some of these do not exist, at least on Linux.
        if os.path.exists(path) and not os.access(path, os.W_OK):
            sys.exit("Error, cannot write to '%s', but that is required." % path)

    from pip._internal.cli.main import main as _main

    sys.exit(_main())


if __name__ == "__main__":
    main()
