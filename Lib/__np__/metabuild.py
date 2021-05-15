import os
import json
import setuptools.build_meta
import pip._vendor.pkg_resources as pkg_resources

import __np__.packaging

class ManagedBackend():
    def prepare_metadata_for_build_wheel(
        self, metadata_directory, config_settings=None
    ):
        with open(os.path.join("..", "np_script.json")) as f:
            metadata = json.load(f)
        package_dir_name = os.path.basename(os.getcwd())
        package_name = package_dir_name[0:package_dir_name.rfind("_")]
        package_metadata = metadata[package_name]
        with open(os.path.join(metadata_directory, "METADATA"), 'w') as f:
            f.write("Metadata-Version: 2.1\n")
            f.write(f"Name: {package_metadata['name']}\n")
            f.write(f"Version: {package_metadata['version']}\n")
            if 'dist_requires' in package_metadata['script_metadata']:
                for dependency in package_metadata['script_metadata']['dist_requires']:
                    f.write(f"Requires-Dist: {dependency}\n")

        return "."

    def build_wheel(
        self,
        wheel_directory,
        config_settings = None,
        metadata_directory = None,
    ):
        with open(os.path.join("..", "np_script.json")) as f:
            metadata = json.load(f)
        package_dir_name = os.path.basename(os.getcwd())
        package_name = package_dir_name[0:package_dir_name.rfind("_")]
        package_metadata = metadata[package_name]

        return __np__.packaging.build_package(package_metadata['name'], package_metadata['version'], package_metadata['script_metadata'], wheel_directory)


managed_build = ManagedBackend()
