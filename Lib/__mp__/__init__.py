import platform

if platform.system() == "Windows":
    from __mp__.windows import *
elif platform.system() == "Linux":
    from __mp__.linux import *
elif platform.system() == "Darwin":
    from __mp__.darwin import *

