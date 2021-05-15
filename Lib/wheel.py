import os
import sys
import sysconfig
import importlib.machinery

# Make the standard wheel, the real wheel module.
# Need to keep a reference alive, or the module will loose all attributes.
if "wheel" in sys.modules:
    _this_module = sys.modules["wheel"]
    del sys.modules["wheel"]

loader = importlib.machinery.SourceFileLoader('wheel', os.path.join(os.path.dirname(__file__), "site-packages", "wheel", "__init__.py"))
wheel = loader.load_module()
sys.modules["wheel"] = wheel
loader.exec_module(wheel)

def our_generic_abi():
    return [wheel.vendored.packaging.tags._normalize_string(sysconfig.get_config_var("SOABI"))]

import wheel.vendored.packaging.tags
wheel.vendored.packaging.tags._generic_abi = our_generic_abi

