# Vendored copy of pyobjtools (https://github.com/Maxwell175/pyobjtools).
# Provides pure-Python replacements for `llvm-nm`, `llvm-objcopy --redefine-syms`,
# and `ar`, used by __mp__ to manipulate static archives without dragging in a
# separate clang/llvm toolchain.
from . import nm, objcopy, ar  # noqa: F401
