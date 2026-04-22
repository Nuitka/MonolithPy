import os
import json
import setuptools.build_meta
import pip._vendor.pkg_resources as pkg_resources
from email.message import Message

import __mp__.packaging

class ManagedBackend():
    def prepare_metadata_for_build_wheel(
        self, metadata_directory, config_settings=None
    ):
        with open(os.path.join("..", "mp_script.json")) as f:
            metadata = json.load(f)
        package_dir_name = os.path.basename(os.getcwd())
        if "_" in package_dir_name:
            package_name = package_dir_name[0:package_dir_name.rfind("_")]
        else:
            package_name = package_dir_name
        package_metadata = metadata[package_name]
        msg = Message()
        msg.add_header("Metadata-Version", "2.1")
        msg.add_header("Name", package_metadata['name'])
        msg.add_header("Version", package_metadata['version'])
        if 'dependencies' in package_metadata['script_metadata']:
            for dependency in package_metadata['script_metadata']['dependencies']:
                msg.add_header("Requires-Dist", f"mpy-dep-{dependency}")
        if 'dist_requires' in package_metadata['script_metadata']:
            for dependency in package_metadata['script_metadata']['dist_requires']:
                msg.add_header("Requires-Dist", dependency)
        with open(os.path.join(metadata_directory, "METADATA"), 'w') as f:
            f.write(msg.as_string())

        return "."

    def build_wheel(
        self,
        wheel_directory,
        config_settings = None,
        metadata_directory = None,
    ):
        with open(os.path.join("..", "mp_script.json")) as f:
            metadata = json.load(f)
        package_dir_name = os.path.basename(os.getcwd())
        if "_" in package_dir_name:
            package_name = package_dir_name[0:package_dir_name.rfind("_")]
        else:
            package_name = package_dir_name
        package_metadata = metadata[package_name]

        return __mp__.packaging.build_package(package_metadata['name'], package_metadata['version'], package_metadata['script_metadata'], wheel_directory)


managed_build = ManagedBackend()
