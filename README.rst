This is Nuitka-Python
=====================

This is a fork of CPython, that is called Nuitka-Python, and diverges in various
ways from the original.

Our immediate goals are:

- ☑  Windows: Add static linking (performance and lock down)
- ☑  Windows: Reenable Windows 7 support (because why not)
- ☑  LTO linking on Windows (performance and deployment size)
- ☑  Automatic patches for packages built from pip
- ☐  Support all major packages and provide upstream guidance.

Installation
============

Currently, you have to build Nuitka-Python from source code. It is questionable
if deploying binaries makes sense, as you will have to compile from source code
everything else that is installed afterwards, and the ``python.exe`` will be
self-modifying with pip installs.

Use the following command in the root of a Nuitka-Python repository checkout:

.. code:: sh

    build.bat -x64

This produces a portable Python installation in the ``output`` folder. It has a
``pip`` and ``setuptools``, but not ``wheel`` out of the box. Nuitka-Python
supports these all of these though, with differences to standard CPython that
make it essentially always compile from source. Esp. the architecture of
Nuitka-Python will be different, so binary wheels uploaded to PyPI cannot be
installed, which will make it fallback to compiling from source.

The ``output`` folder can be moved freely, even to other machines. When you
install via ``python.exe -m pip`` however, it will be self-modifying the
``python.exe`` to include the newly installed packages with static linking.


Copyright and License Information
---------------------------------

Copyright (c) 2021-2022 Nuitka Organization contributors. All rights reserved.

Copyright (c) 2001-2021 Python Software Foundation.  All rights reserved.

Copyright (c) 2000 BeOpen.com.  All rights reserved.

Copyright (c) 1995-2001 Corporation for National Research Initiatives.  All
rights reserved.

Copyright (c) 1991-1995 Stichting Mathematisch Centrum.  All rights reserved.

See the file "LICENSE" for information on the history of this software, terms &
conditions for usage, and a DISCLAIMER OF ALL WARRANTIES.

This Python distribution contains *no* GNU General Public License (GPL) code,
so it may be used in proprietary projects.  There are interfaces to some GNU
code but these are entirely optional.

All trademarks referenced herein are property of their respective holders.
