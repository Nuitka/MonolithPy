#!/bin/bash

set -e
set -x

if [ "$arch" = "" ]; then
  if [ "$(arch)" = "arm64" ]; then
    arch=arm64
  else
    arch=x86_64
  fi
fi

echo Building for architecture $arch

# The dependencies must be outside of the build folder because
# the python build process ends up running a find -delete that
# happens to also delete all the static libraries that we built.
export "PREFIX=$(pwd)/../Nuitka-Python-Deps"
export "PYTHON_BASE=$(pwd)"
export "PKG_CONFIG_PATH=${PREFIX}/lib/pkgconfig:${PREFIX}/share/pkgconfig"
export "CFLAGS=-arch $arch -mmacosx-version-min=10.9 -I${PREFIX}/include -I${PYTHON_BASE}/Include -fPIC"
export "CXXFLAGS=-arch $arch -mmacosx-version-min=10.9 -I${PREFIX}/include -fPIC"
export "LDFLAGS=-arch $arch -L${PREFIX}/lib"
export "MACOSX_DEPLOYMENT_TARGET=10.9"

# Allow to overload the compiler used via CC environment variable
if [ "$CC" = "" ]
then
  export CC=clang
  export CXX=clang++
else
  export CXX=`echo "$CC" | sed -e 's#cc#++#'`
fi

export CC
export CXX

ELEVATE=
if [ ! -w "$(dirname "$target")" ]
then
  export "ELEVATE=sudo --preserve-env=CC,CXX"
  $ELEVATE echo
fi

# Preparing embedded resources
mkdir -p Embedded/embed_data/vfs/ssl
curl -L https://mkcert.org/generate/ | python3 -c "import sys; [sys.stdout.buffer.write(line.decode('utf-8').encode('ascii', errors='backslashreplace')) for line in sys.stdin.buffer]" > Embedded/embed_data/vfs/ssl/cert.pem
python3 Lib/mkembed.py Embedded Embedded/embed_data
$CC -c -O0 -g -o Embedded/np_embed.o Embedded/np_embed.c -IInclude
$CC -c -o Embedded/np_embed_data.o Embedded/np_embed_data.c
ar rcs ${PREFIX}/lib/libnp_embed.a Embedded/np_embed.o Embedded/np_embed_data.o


mkdir -p ${PREFIX}/lib

if [ ! -h ${PREFIX}/lib64 ]; then
  ln -s lib ${PREFIX}/lib64
fi

mkdir -p dep-build
cd dep-build

if [ ! -d ncurses-6.4 ]; then
curl https://ftp.gnu.org/gnu/ncurses/ncurses-6.4.tar.gz -o ncurses.tar.gz
tar -xf ncurses.tar.gz
cd ncurses-6.4
./configure --prefix=${PREFIX} --disable-shared --enable-termcap --enable-widec --enable-getcap
make -j$(sysctl -n hw.ncpu)
make install
for header in ${PREFIX}/include/ncursesw/*; do
    ln -s ncursesw/$(basename $header) ${PREFIX}/include/;
done
cd ..
fi

if [ ! -d editline-1.17.1 ]; then
curl -L https://github.com/troglobit/editline/releases/download/1.17.1/editline-1.17.1.tar.gz -o editline.tar.gz
tar -xf editline.tar.gz
cd editline-1.17.1
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d sqlite-autoconf-3440000 ]; then
curl https://sqlite.org/2023/sqlite-autoconf-3440000.tar.gz -o sqlite.tar.gz
tar -xf sqlite.tar.gz
cd sqlite-autoconf-3440000
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d openssl-3.1.8 ]; then
curl -L https://www.openssl.org/source/openssl-3.1.8.tar.gz -o openssl.tar.gz
tar -xf openssl.tar.gz
cd openssl-3.1.8
export "CPPINCLUDES=$PYTHON_BASE/Include"
./Configure --prefix=${PREFIX} --libdir=lib darwin64-$arch enable-ec_nistp_64_gcc_128 no-shared no-tests --openssldir=/vfs/ssl
if [[ "$OSTYPE" == "darwin"* ]]; then
    find . \( -iname '*.h.in' -o -iname '*.h' -o -iname '*.c' \) | xargs sed -i '' '1s/^/#include "np_embed.h"\n\'$'\n/g'
else
    find . \( -iname '*.h.in' -o -iname '*.h' -o -iname '*.c' \) | xargs sed -i '1s/^/#include "np_embed.h"\n\'$'\n/g'
fi
#make depend all -j$(sysctl -n hw.ncpu)
make install_dev -j$(sysctl -n hw.ncpu)
unset CPPINCLUDES
cd ..
fi

if [ ! -d bzip2-1.0.8 ]; then
curl -L https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz -o bzip2.tar.gz
tar -xf bzip2.tar.gz
cd bzip2-1.0.8
make install "PREFIX=$PREFIX" -j$(sysctl -n hw.ncpu)
cd ..
fi

if [ ! -d xz-5.4.5 ]; then
curl -L https://downloads.sourceforge.net/project/lzmautils/xz-5.4.5.tar.gz -o xz.tar.gz
tar -xf xz.tar.gz
cd xz-5.4.5
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d libffi-3.4.6 ]; then
curl -L https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz -o libffi.tar.gz
tar -xf libffi.tar.gz
cd libffi-3.4.6
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d zlib-latest ]; then
curl -L https://www.zlib.net/current/zlib.tar.gz -o zlib.tar.gz
tar -xf zlib.tar.gz
mv zlib-* zlib-latest
cd zlib-latest
./configure --prefix=${PREFIX} --static
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d libxcrypt-4.4.36 ]; then
curl -L https://github.com/besser82/libxcrypt/releases/download/v4.4.36/libxcrypt-4.4.36.tar.xz -o libxcrypt.tar.xz
tar -xf libxcrypt.tar.xz
cd libxcrypt-4.4.36
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d libpng-1.6.39 ]; then
curl -L http://downloads.sourceforge.net/project/libpng/libpng16/1.6.39/libpng-1.6.39.tar.xz -o libpng.tar.gz
tar -xf libpng.tar.gz
cd libpng-1.6.39
./configure --prefix=${PREFIX} --disable-shared --with-zlib-prefix=${PREFIX}
make pnglibconf.h
sed -i '' -e 's/define PNG_ZLIB_VERNUM 0x[0-9a-z][0-9a-z][0-9a-z][0-9a-z]/define PNG_ZLIB_VERNUM 0/g' pnglibconf.h
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d harfbuzz-8.3.0 ]; then
curl -L https://github.com/harfbuzz/harfbuzz/releases/download/8.3.0/harfbuzz-8.3.0.tar.xz -o harfbuzz.tar.gz
tar -xf harfbuzz.tar.gz
cd harfbuzz-8.3.0
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d freetype-2.13.2 ]; then
curl -L https://download.savannah.gnu.org/releases/freetype/freetype-2.13.2.tar.gz -o freetype.tar.gz
tar -xf freetype.tar.gz
cd freetype-2.13.2
./configure --prefix=${PREFIX} --disable-shared --with-brotli=no
make -j$(sysctl -n hw.ncpu)
make install
cd ..

cd harfbuzz-8.3.0
export FREETYPE_CFLAGS=-I/Users/m1/Nuitka-Python/../Nuitka-Python-Deps/include/freetype2
export "FREETYPE_LIBS=-L/Users/m1/Nuitka-Python/../Nuitka-Python-Deps/lib -lfreetype"
./configure --prefix=/Users/m1/Nuitka-Python/../Nuitka-Python-Deps --disable-shared --with-freetype=yes
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d tcl8.6.15 ]; then
curl -L http://downloads.sourceforge.net/project/tcl/Tcl/8.6.15/tcl8.6.15-src.tar.gz -o tcl.tar.gz
tar -xf tcl.tar.gz
cd tcl8.6.15/unix
./configure --prefix=${PREFIX} --enable-shared=no --enable-threads
make -j$(sysctl -n hw.ncpu)
make install
cd ../..
fi

if [ ! -d expat-2.5.0 ]; then
curl -L https://github.com/libexpat/libexpat/releases/download/R_2_5_0/expat-2.5.0.tar.gz -o expat.tar.gz
tar -xf expat.tar.gz
cd expat-2.5.0
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d tk8.6.15 ]; then
curl -L http://downloads.sourceforge.net/project/tcl/Tcl/8.6.15/tk8.6.15-src.tar.gz -o tk.tar.gz
tar -xf tk.tar.gz
cd tk8.6.15/unix
./configure --prefix=${PREFIX} --enable-shared=no --enable-threads --with-tcl=${PREFIX}/lib --enable-aqua
make -j$(sysctl -n hw.ncpu)
make install
cd ../..
fi

if [ ! -d mpdecimal-4.0.0 ]; then
curl -L https://www.bytereef.org/software/mpdecimal/releases/mpdecimal-4.0.0.tar.gz -o mpdecimal.tar.gz
tar -xf mpdecimal.tar.gz
cd mpdecimal-4.0.0
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d libb2-0.98.1 ]; then
curl -L https://github.com/BLAKE2/libb2/releases/download/v0.98.1/libb2-0.98.1.tar.gz -o libb2.tar.gz
tar -xf libb2.tar.gz
cd libb2-0.98.1
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

cd ..



long_version=$(git branch --show-current 2>/dev/null || git symbolic-ref --short HEAD)
short_version=$(echo $long_version | sed -e 's#\.##')

# Have this as a standard path. We are not yet relocatable, but that will come hopefully.
target=~/Library/Nuitka-Python${short_version}-$arch

if [ ! -z "$1" ]
then
  target="$1"
fi

cp Modules/Setup.macos Modules/Setup

export "LDFLAGS=-L${PREFIX}/lib"

./configure "--prefix=$target" --disable-shared --enable-ipv6 --enable-unicode=ucs4 \
  --enable-optimizations --with-lto --with-computed-gotos --with-fpectl --without-readline \
  --with-system-expat --with-system-libmpdec \
  CC="$CC" \
  CXX="$CXX" \
  CFLAGS="-g $CFLAGS" \
  LDFLAGS="-arch $arch -g -Xlinker $LDFLAGS" \
  LIBS="-lffi -lbz2 -lsqlite3 -llzma -lnp_embed -lssl -lcrypto " \
  ax_cv_c_float_words_bigendian=no

make -j 32 \
        PROFILE_TASK='./Lib/test/regrtest.py -j 8 -x test_bsddb3 test_compiler test_cpickle test_cprofile test_dbm_dumb test_dbm_ndbm test_distutils test_ensurepip test_gdb test_io test_linuxaudiodev test_multiprocessing test_ossaudiodev test_platform test_pydoc test_socketserver test_subprocess test_sundry test_thread test_threaded_import test_threadedtempfile test_threading test_threading_local test_threadsignals test_xmlrpc test_zipfile' \
        profile-opt

make build_all_merge_profile

# Delayed deletion of old installation, to avoid having it not there for testing purposes
# while compiling, which is slow due to PGO beign applied.
$ELEVATE rm -rf "$target" && $ELEVATE make install

# Make sure to have pip installed.
$ELEVATE mv "$target/lib/python${long_version}/pip.py" "$target/lib/python${long_version}/pip.py.bak" && \
    $ELEVATE "$target/bin/python${long_version}" -m ensurepip && \
    $ELEVATE mv "$target/lib/python${long_version}/pip.py.bak" "$target/lib/python${long_version}/pip.py"

# Copy embedded data
$ELEVATE mkdir -p "$target/embed_data"
$ELEVATE mv ${PREFIX}/lib/libnp_embed.a "$target/lib/libnp_embed.a"
$ELEVATE cp -r Embedded/embed_data "$target/embed_data"

# Copy over the compiled dependencies.
$ELEVATE mkdir -p "$target/dependency_libs"
$ELEVATE cp -r "$(pwd)/../Nuitka-Python-Deps" "$target/dependency_libs/base"
$ELEVATE ln -s base "$target/dependency_libs/bzip2"
$ELEVATE ln -s base "$target/dependency_libs/editline"
$ELEVATE ln -s base "$target/dependency_libs/expat"
$ELEVATE ln -s base "$target/dependency_libs/freetype"
$ELEVATE ln -s base "$target/dependency_libs/harfbuzz"
$ELEVATE ln -s base "$target/dependency_libs/b2"
$ELEVATE ln -s base "$target/dependency_libs/png"
$ELEVATE ln -s base "$target/dependency_libs/xcrypt"
$ELEVATE ln -s base "$target/dependency_libs/mpdecimal"
$ELEVATE ln -s base "$target/dependency_libs/ncurses"
$ELEVATE ln -s base "$target/dependency_libs/openssl"
$ELEVATE ln -s base "$target/dependency_libs/sqlite"
$ELEVATE ln -s base "$target/dependency_libs/tcltk"
$ELEVATE ln -s base "$target/dependency_libs/xz"
$ELEVATE ln -s base "$target/dependency_libs/zlib"

$ELEVATE "$target/bin/python${long_version}" -m rebuildpython

