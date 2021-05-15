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

export "LTO_OPTS=-flto=thin"
export "LTO=1"
if [[ $1 == "--no-lto" ]]; then
    export "LTO_OPTS="
    export "LTO=0"
    shift
fi

# The dependencies must be outside of the build folder because
# the python build process ends up running a find -delete that
# happens to also delete all the static libraries that we built.
export "PREFIX=$(pwd)/../Nuitka-Python-Deps"
export "PYTHON_BASE=$(pwd)"
export "PKG_CONFIG_PATH=${PREFIX}/lib/pkgconfig:${PREFIX}/share/pkgconfig"
export "CFLAGS=-arch $arch -mmacosx-version-min=10.9 -I${PREFIX}/include -I${PYTHON_BASE}/Include -fPIC ${LTO_OPTS}"
export "CXXFLAGS=-arch $arch -mmacosx-version-min=10.9 -I${PREFIX}/include -I${PYTHON_BASE}/Include -fPIC ${LTO_OPTS}"
export "CPPFLAGS=-I${PREFIX}/include -I${PYTHON_BASE}/Include"
export "LDFLAGS=-arch $arch -L${PREFIX}/lib -lnp_embed ${LTO_OPTS}"
export "CCexe_LDFLAGS=-arch $arch -L${PREFIX}/lib -lnp_embed -I${PYTHON_BASE}/Include"
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

# Have this as a standard path. We are not yet relocatable, but that will come hopefully.
target=~/Library/Nuitka-Python${short_version}-$arch

if [ ! -z "$1" ]
then
  target="$1"
fi

ELEVATE=
if [ ! -w "$(dirname "$target")" ]
then
  export "ELEVATE=sudo --preserve-env=CC,CXX"
  $ELEVATE echo
fi

mkdir -p ${PREFIX}/lib

# Preparing embedded resources
mkdir -p Embedded/embed_data/vfs/ssl
curl -L https://mkcert.org/generate/ | python3 -c "import sys; [sys.stdout.buffer.write(line.decode('utf-8').encode('ascii', errors='backslashreplace')) for line in sys.stdin.buffer]" > Embedded/embed_data/vfs/ssl/cert.pem
python3 Lib/mkembeddata.py Embedded Embedded/embed_data
$CC -c -g -o Embedded/np_embed.o Embedded/np_embed.c -IInclude
$CC -c -o Embedded/np_embed_data.o Embedded/np_embed_data.c
ar rcs ${PREFIX}/lib/libnp_embed.a Embedded/np_embed.o Embedded/np_embed_data.o

if [ ! -h ${PREFIX}/lib64 ]; then
  ln -s lib ${PREFIX}/lib64
fi

mkdir -p dep-build
cd dep-build

download_file() {
  local url="$1"
  local filename="$2"
  for i in {1..5}; do
    curl -L "$url" -o "$filename" && return 0 || sleep 5
  done
  echo "Failed to download '$url' to '$filename' after 5 retries."
  return 1
}

if [ ! -d ncurses-6.4 ]; then
download_file https://ftp.gnu.org/gnu/ncurses/ncurses-6.4.tar.gz ncurses.tar.gz
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
download_file https://github.com/troglobit/editline/releases/download/1.17.1/editline-1.17.1.tar.gz editline.tar.gz
tar -xf editline.tar.gz
cd editline-1.17.1
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d sqlite-autoconf-3440000 ]; then
download_file https://sqlite.org/2023/sqlite-autoconf-3440000.tar.gz sqlite.tar.gz
tar -xf sqlite.tar.gz
cd sqlite-autoconf-3440000
export "CFLAGS_bak=$CFLAGS"
export "CPPFLAGS_bak=$CPPFLAGS"
export "CFLAGS=$CFLAGS -DSQLITE_ENABLE_COLUMN_METADATA"
export "CPPFLAGS=$CPPFLAGS -DSQLITE_ENABLE_COLUMN_METADATA"
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
export "CFLAGS=$CFLAGS_bak"
export "CPPFLAGS=$CPPFLAGS_bak"
cd ..
fi

if [ ! -d openssl-3.1.8 ]; then
download_file https://www.openssl.org/source/openssl-3.1.8.tar.gz openssl.tar.gz
tar -xf openssl.tar.gz
cd openssl-3.1.8
export "CPPINCLUDES=$PYTHON_BASE/Include"
./Configure --prefix=${PREFIX} --libdir=lib darwin64-$arch enable-ec_nistp_64_gcc_128 no-shared no-tests --openssldir=/vfs/ssl
find . \( -iname '*.h.in' -o -iname '*.h' -o -iname '*.c' -o -iname '*.cpp' -o -iname '*.cxx' \) | xargs sed -i '' '1s/^/#include "np_embed.h"\n\'$'\n/g'
#make depend all -j$(sysctl -n hw.ncpu)
make install_dev -j$(sysctl -n hw.ncpu)
unset CPPINCLUDES
cd ..
fi

if [ ! -d bzip2-bzip2-1.0.8 ]; then
download_file https://gitlab.com/bzip2/bzip2/-/archive/bzip2-1.0.8/bzip2-bzip2-1.0.8.tar.gz bzip2.tar.gz
tar -xf bzip2.tar.gz
cd bzip2-bzip2-1.0.8
make install "PREFIX=$PREFIX" -j$(sysctl -n hw.ncpu)
cd ..
fi

if [ ! -d xz-5.4.5 ]; then
download_file https://downloads.sourceforge.net/project/lzmautils/xz-5.4.5.tar.gz xz.tar.gz
tar -xf xz.tar.gz
cd xz-5.4.5
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d libffi-3.5.1 ]; then
download_file https://github.com/libffi/libffi/releases/download/v3.5.1/libffi-3.5.1.tar.gz libffi.tar.gz
tar -xf libffi.tar.gz
cd libffi-3.5.1
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d zlib-latest ]; then
download_file https://www.zlib.net/current/zlib.tar.gz zlib.tar.gz
tar -xf zlib.tar.gz
mv zlib-* zlib-latest
cd zlib-latest
./configure --prefix=${PREFIX} --static
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d libxcrypt-4.4.36 ]; then
download_file https://github.com/besser82/libxcrypt/releases/download/v4.4.36/libxcrypt-4.4.36.tar.xz libxcrypt.tar.xz
tar -xf libxcrypt.tar.xz
cd libxcrypt-4.4.36
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d libpng-1.6.46 ]; then
download_file http://downloads.sourceforge.net/project/libpng/libpng16/1.6.46/libpng-1.6.46.tar.xz libpng.tar.gz
tar -xf libpng.tar.gz
cd libpng-1.6.46
./configure --prefix=${PREFIX} --disable-shared --with-zlib-prefix=${PREFIX}
make pnglibconf.h
find . \( -iname '*.h.in' -o -iname '*.h' -o -iname '*.c' -o -iname '*.cpp' -o -iname '*.cxx' \) | xargs sed -i '' '1s/^/#include "np_embed.h"\n\'$'\n/g'
sed -i '' -e 's/define PNG_ZLIB_VERNUM 0x[0-9a-z][0-9a-z][0-9a-z][0-9a-z]/define PNG_ZLIB_VERNUM 0/g' pnglibconf.h
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d harfbuzz-8.3.0 ]; then
download_file https://github.com/harfbuzz/harfbuzz/releases/download/8.3.0/harfbuzz-8.3.0.tar.xz harfbuzz.tar.gz
tar -xf harfbuzz.tar.gz
cd harfbuzz-8.3.0
find . \( -iname '*.h.in' -o -iname '*.h' -o -iname '*.c' -o -iname '*.cpp' -o -iname '*.cxx' \) | xargs sed -i '' '1s/^/#include "np_embed.h"\n\'$'\n/g'
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d freetype-2.13.3 ]; then
download_file http://downloads.sourceforge.net/project/freetype/freetype2/2.13.3/freetype-2.13.3.tar.xz freetype.tar.gz
tar -xf freetype.tar.gz
cd freetype-2.13.3
find . \( -iname '*.h.in' -o -iname '*.h' -o -iname '*.c' -o -iname '*.cpp' -o -iname '*.cxx' \) | xargs sed -i '' '1s/^/#include "np_embed.h"\n\'$'\n/g'
./configure --prefix=${PREFIX} --disable-shared --with-brotli=no
make -j$(sysctl -n hw.ncpu)
make install
cd ..

cd harfbuzz-8.3.0
export FREETYPE_CFLAGS=-I${PREFIX}/include/freetype2
export "FREETYPE_LIBS=-L${PREFIX}/lib -lfreetype"
./configure --prefix=${PREFIX} --disable-shared --with-freetype=yes
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d tcl8.6.15 ]; then
download_file http://downloads.sourceforge.net/project/tcl/Tcl/8.6.15/tcl8.6.15-src.tar.gz tcl.tar.gz
tar -xf tcl.tar.gz
cd tcl8.6.15
rm -rf pkgs/tdbc* pkgs/sqlite*
cd unix
./configure --prefix=${PREFIX} --enable-shared=no --enable-threads
make -j$(sysctl -n hw.ncpu)
make install
cd ../..
fi

if [ ! -d expat-2.5.0 ]; then
download_file https://github.com/libexpat/libexpat/releases/download/R_2_5_0/expat-2.5.0.tar.gz expat.tar.gz
tar -xf expat.tar.gz
cd expat-2.5.0
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d tk8.6.15 ]; then
download_file http://downloads.sourceforge.net/project/tcl/Tcl/8.6.15/tk8.6.15-src.tar.gz tk.tar.gz
tar -xf tk.tar.gz
cd tk8.6.15/unix
./configure --prefix=${PREFIX} --enable-shared=no --enable-threads --with-tcl=${PREFIX}/lib --enable-aqua
make -j$(sysctl -n hw.ncpu)
make install
cd ../..
fi

if [ ! -d mpdecimal-4.0.0 ]; then
download_file https://www.bytereef.org/software/mpdecimal/releases/mpdecimal-4.0.0.tar.gz mpdecimal.tar.gz
tar -xf mpdecimal.tar.gz
cd mpdecimal-4.0.0
./configure --prefix=${PREFIX} --disable-shared
make -j$(sysctl -n hw.ncpu)
make install
cd ..
fi

if [ ! -d libb2-0.98.1 ]; then
download_file https://github.com/BLAKE2/libb2/releases/download/v0.98.1/libb2-0.98.1.tar.gz libb2.tar.gz
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

cp Modules/Setup.macos Modules/Setup

export "LDFLAGS=-L${PREFIX}/lib"

if [[ "$LTO" == "1" ]]; then
    LTO_BUILD_OPT=--with-lto
fi

./configure "--prefix=$target" --disable-shared --enable-ipv6 --enable-unicode=ucs4 \
  --enable-optimizations $LTO_BUILD_OPT --with-computed-gotos --with-fpectl --without-readline \
  --with-system-expat --with-system-libmpdec \
  CC="$CC" \
  CXX="$CXX" \
  CFLAGS="-g $CFLAGS" \
  LDFLAGS="-arch $arch -g -Xlinker $LDFLAGS ${LTO_OPTS}" \
  LIBS="-lffi -lbz2 -lsqlite3 -llzma -lnp_embed -lssl -lcrypto " \
  ax_cv_c_float_words_bigendian=no \
  ___ORIG_DEPS_PREFIX=${PREFIX}___

make -j $(sysctl -n hw.ncpu) \
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
$ELEVATE mv ${PREFIX}/lib/libnp_embed.a "$target/lib/libnp_embed.a"

$ELEVATE mkdir -p "$target/Embedded"
# The object file usually gets deleted during the build, so make sure to recompile here just in case.
rm -f Embedded/np_embed.o
$CC -c -g -o Embedded/np_embed.o Embedded/np_embed.c -IInclude
$ELEVATE cp -r "Embedded/np_embed.o" "$target/Embedded/"
$ELEVATE cp -r "Embedded/embed_data" "$target/Embedded/"

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

find "$target" \( -iname '*.la' -o -iname '*.pc' -o -iname '__pycache__' -o -iname 'link.json' \) | xargs $ELEVATE rm -rf

$ELEVATE "$target/bin/python${long_version}" -m rebuildpython
