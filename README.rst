This is MonolithPy
=====================

MonolithPy is a fork of CPython (currently based on Python 3.13) that produces
fully statically linked Python interpreters. It is designed for creating portable,
self-contained Python distributions.

Goals
=====

- ☑  Static linking on all platforms (performance and lock down)
- ☑  Windows 7 support retained
- ☑  LTO (Link Time Optimization) for performance and smaller binaries
- ☑  Automatic source compilation for pip installs
- ☑  Embedded resources (SSL certificates, VFS)
- ☑  Package-wide symbol renaming for compatibility
- ☐  Support all major packages and provide upstream guidance

Installation
============

MonolithPy must be built from source. Deploying binaries is impractical because
installing packages afterward requires recompiling the interpreter, so you
need a working build environment regardless.

Windows
-------

Prerequisites: Visual Studio with C++ build tools.

.. code:: sh

    build.bat -x64

Options:

- ``-x64`` - Build for 64-bit Windows (default)
- ``-x86`` - Build for 32-bit Windows
- ``--pgo`` - Enable Profile-Guided Optimization
- ``-r`` - Rebuild from scratch

This produces a portable Python installation in the ``output`` folder (or
``output32`` for 32-bit builds).

Linux
-----

Prerequisites: ``build-essential``, ``libc6-dev``, ``gperf`` (on Debian/Ubuntu).
The build script will attempt to install these automatically.

.. code:: sh

    ./build.sh [target_directory]

The script downloads and compiles all dependencies, then builds Python with
LTO and PGO enabled. The default target is ``/opt/monolithpy<version>``.

Example with custom target:

.. code:: sh

    ./build.sh ~/my-python

macOS
-----

Prerequisites: Xcode command line tools.

.. code:: sh

    ./build.mac.sh [target_directory]

Options:

- ``--no-lto`` - Disable Link Time Optimization (faster build)

The script auto-detects architecture (arm64 or x86_64). The default target is
``~/Library/MonolithPy<version>-<arch>``.

Example:

.. code:: sh

    ./build.mac.sh ~/my-python

Cross-architecture build (Intel on Apple Silicon):

.. code:: sh

    arch -x86_64 ./build.mac.sh

Output Structure
================

After building, the output folder contains:

- ``python`` / ``python.exe`` - The interpreter binary
- ``lib/`` - Standard library and installed packages
- ``include/`` - Python headers for building extensions
- ``dependency_libs/`` - Compiled dependency libraries
- ``Embedded/`` - Embedded resource data and object files

The ``output`` folder can be moved freely, even to other machines with the
same OS/architecture. When you install packages via ``python -m pip``, the
interpreter will self-modify to statically link new native extensions.

Package Installation
====================

Installing packages works normally, but with important differences:

.. code:: sh

    ./output/python -m pip install numpy

What happens:

1. pip downloads the source distribution (wheels are incompatible)
2. MonolithPy-specific build scripts may be fetched from the packages repo
3. The package is compiled with static linking
4. ``rebuildpython`` is invoked to relink the interpreter
5. The new interpreter includes the extension as a builtin module

Not all packages are supported. Packages requiring dynamic loading or
incompatible build systems may fail. The goal is to support major packages
and provide guidance for upstream fixes.


Architecture Differences from CPython
=====================================

Static Linking
--------------

Unlike standard CPython which uses dynamic libraries (.so/.dll/.dylib),
MonolithPy statically links all extension modules and dependencies directly
into the interpreter binary. This produces a single, self-contained executable
that can be deployed without external library dependencies.

Self-Modifying Executable
-------------------------

When you install packages via pip, MonolithPy automatically relinks the
interpreter to incorporate newly installed native extensions. The
``rebuildpython`` module scans for static libraries with Python init
functions and regenerates the interpreter binary. This means:

- Installing a package may trigger a recompile of the interpreter
- The ``python`` executable will be replaced with a new version
- Link-time information is stored in ``link.json``

Embedded Resources
------------------

MonolithPy includes an embedded virtual filesystem (VFS) for resources like
SSL certificates. The ``mp_embed`` module provides file I/O interception to
serve embedded files transparently. This allows the interpreter to function
without external data files.

Symbol Renaming
---------------

To avoid symbol conflicts when linking multiple static libraries, MonolithPy
implements package-wide symbol analysis and renaming. This handles cases where
different libraries (e.g., numpy, pybind11, cython) have conflicting symbol
names.

Build Dependencies
------------------

MonolithPy builds all dependencies from source with static linking enabled.
This includes:

- OpenSSL, SQLite, zlib, bzip2, xz/lzma
- libffi, mpdecimal, libb2
- ncurses, editline
- Tcl/Tk (with X11 dependencies on Linux)
- harfbuzz, freetype, fontconfig (for font rendering)


Copyright and License Information
---------------------------------

Copyright (c) 2021-2025 Nuitka Organization contributors. All rights reserved.

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
