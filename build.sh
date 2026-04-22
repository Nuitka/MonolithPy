#!/bin/bash

set -x


# Install Debian dependencies.
# TODO: Support Fedora/CentOS/etc. as well.
if command -v apt &> /dev/null
then
  pkgs='build-essential libc6-dev gperf'
  install=false
  for pkg in $pkgs; do
    status="$(dpkg-query -W --showformat='${db:Status-Status}' "$pkg" 2>&1)"
    if [ ! $? = 0 ] || [ ! "$status" = installed ]; then
      install=true
      break
    fi
  done
  if "$install"; then
    sudo apt install $pkgs
  fi
fi

set -e
set -x

# The dependencies must be outside of the build folder because
# the python build process ends up running a find -delete that
# happens to also delete all the static libraries that we built.
export "PREFIX=$(pwd)/../MonolithPy-Deps"
export "PYTHON_BASE=$(pwd)"
export "CFLAGS=-I${PREFIX}/include -I${PYTHON_BASE}/Include -fPIC -flto -fuse-linker-plugin -fno-fat-lto-objects"
export "CXXFLAGS=-I${PREFIX}/include -I${PYTHON_BASE}/Include -fPIC -flto -fuse-linker-plugin -fno-fat-lto-objects"
export "CPPFLAGS=-I${PREFIX}/include -I${PYTHON_BASE}/Include"
export "LDFLAGS=-L${PREFIX}/lib -lmp_embed -lm -flto=auto -fuse-linker-plugin -fno-fat-lto-objects"
export "CCexe_LDFLAGS=-L${PREFIX}/lib -lmp_embed -I${PYTHON_BASE}/Include"
export "PKG_CONFIG_PATH=${PREFIX}/lib/pkgconfig:${PREFIX}/share/pkgconfig"


# Allow to overload the compiler used via CC environment variable
if [ "$CC" = "" ]
then
  CC=gcc
  CXX=g++
else
  CXX=`echo "$CC" | sed -e 's#cc#++#'`
fi

export CC
export CXX

ELEVATE=
if [ ! -w "$(dirname "$target")" ]
then
  export "ELEVATE=sudo \"CC=$CC\" \"CXX=$CXX\""
  sudo echo
fi

mkdir -p ${PREFIX}/lib
mkdir -p ${PREFIX}/include

cp Include/mp_embed.h ${PREFIX}/include/

# Preparing embedded resources
mkdir -p Embedded/embed_data/vfs/ssl
curl -L https://mkcert.org/generate/ | python3 -c "import sys; [sys.stdout.buffer.write(line.decode('utf-8').encode('ascii', errors='backslashreplace')) for line in sys.stdin.buffer]" > Embedded/embed_data/vfs/ssl/cert.pem
python3 Lib/mkembeddata.py Embedded Embedded/embed_data
$CC -c -g -o Embedded/mp_embed.o Embedded/mp_embed.c -IInclude
$CC -c -o Embedded/mp_embed_data.o Embedded/mp_embed_data.c
ar rcs ${PREFIX}/lib/libmp_embed.a Embedded/mp_embed.o Embedded/mp_embed_data.o

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

if [ ! -d ncurses-6.5 ]; then
download_file https://ftp.gnu.org/gnu/ncurses/ncurses-6.5.tar.gz ncurses.tar.gz
tar -xf ncurses.tar.gz
cd ncurses-6.5
./configure --prefix=${PREFIX} --disable-shared --enable-termcap --enable-widec --enable-getcap --without-cxx-binding
make libs -j$(nproc --all)
make install.libs install.includes
for header in ${PREFIX}/include/ncursesw/*; do
    ln -s ncursesw/$(basename $header) ${PREFIX}/include/;
done
cd ..
fi

if [ ! -d editline-f735e4d1d566cac3caa4a5e248179d07f0babefd ]; then
download_file https://github.com/troglobit/editline/archive/f735e4d1d566cac3caa4a5e248179d07f0babefd.tar.gz editline.tar.gz
tar -xf editline.tar.gz
cd editline-f735e4d1d566cac3caa4a5e248179d07f0babefd
./autogen.sh
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
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
make -j$(nproc --all)
make install
export "CFLAGS=$CFLAGS_bak"
export "CPPFLAGS=$CPPFLAGS_bak"
cd ..
fi

if [ ! -d openssl-3.5.4 ]; then
download_file https://www.openssl.org/source/openssl-3.5.4.tar.gz openssl.tar.gz
tar -xf openssl.tar.gz
cd openssl-3.5.4
export "CPPINCLUDES=$PYTHON_BASE/Include"
./Configure --prefix=${PREFIX} --libdir=lib linux-x86_64 enable-ec_nistp_64_gcc_128 no-shared no-tests --openssldir=/vfs/ssl
find . \( -iname '*.h.in' -o -iname '*.h' -o -iname '*.c' -o -iname '*.cc' -o -iname '*.cpp' -o -iname '*.cxx' \) | xargs sed -i '1s/^/#include "mp_embed.h"\n\'$'\n/g'
make install_dev -j$(nproc --all)
unset CPPINCLUDES
cd ..
fi

if [ ! -d bzip2-bzip2-1.0.8 ]; then
download_file https://gitlab.com/bzip2/bzip2/-/archive/bzip2-1.0.8/bzip2-bzip2-1.0.8.tar.gz bzip2.tar.gz
tar -xf bzip2.tar.gz
cd bzip2-bzip2-1.0.8
make install "PREFIX=$PREFIX" -j$(nproc --all)
cd ..
fi

if [ ! -d util-linux-2.39 ]; then
download_file https://mirrors.edge.kernel.org/pub/linux/utils/util-linux/v2.39/util-linux-2.39.tar.gz util-linux.tar.gz
tar -xf util-linux.tar.gz
cd util-linux-2.39
./configure --prefix=${PREFIX} --disable-shared --disable-all-programs --enable-libuuid
make -j$(nproc --all)
make install
cp ./libuuid/src/uuid.h ${PREFIX}/include/
cd ..
fi

if [ ! -d xz-5.4.5 ]; then
download_file https://downloads.sourceforge.net/project/lzmautils/xz-5.4.5.tar.gz xz.tar.gz
tar -xf xz.tar.gz
cd xz-5.4.5
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d libffi-3.4.6 ]; then
download_file https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz libffi.tar.gz
tar -xf libffi.tar.gz
cd libffi-3.4.6
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d zlib-latest ]; then
download_file https://www.zlib.net/current/zlib.tar.gz zlib.tar.gz
tar -xf zlib.tar.gz
mv zlib-* zlib-latest
cd zlib-latest
./configure --prefix=${PREFIX} --static
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d libpng-1.6.39 ]; then
download_file http://downloads.sourceforge.net/project/libpng/libpng16/1.6.39/libpng-1.6.39.tar.xz libpng.tar.gz
tar -xf libpng.tar.gz
cd libpng-1.6.39
./configure --prefix=${PREFIX} --disable-shared
make pnglibconf.h
find . \( -iname '*.h.in' -o -iname '*.h' -o -iname '*.c' -o -iname '*.cc' -o -iname '*.cpp' -o -iname '*.cxx' \) | xargs sed -i '1s/^/#include "mp_embed.h"\n\'$'\n/g'
sed -i -e 's/define PNG_ZLIB_VERNUM 0x[0-9a-z][0-9a-z][0-9a-z][0-9a-z]/define PNG_ZLIB_VERNUM 0/g' pnglibconf.h
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d harfbuzz-8.3.0 ]; then
download_file https://github.com/harfbuzz/harfbuzz/releases/download/8.3.0/harfbuzz-8.3.0.tar.xz harfbuzz.tar.gz
tar -xf harfbuzz.tar.gz
cd harfbuzz-8.3.0
find . \( -iname '*.h.in' -o -iname '*.h' -o -iname '*.c' -o -iname '*.cc' -o -iname '*.cpp' -o -iname '*.cxx' \) | xargs sed -i '1s/^/#include "mp_embed.h"\n\'$'\n/g'
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d xtrans-1.5.0 ]; then
download_file https://xorg.freedesktop.org/releases/individual/lib/xtrans-1.5.0.tar.gz xtrans.tar.gz
tar -xf xtrans.tar.gz
cd xtrans-1.5.0
./configure --prefix=${PREFIX} --datarootdir=${PREFIX}/lib
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d xproto-7.0.31 ]; then
download_file https://www.x.org/archive/individual/proto/xproto-7.0.31.tar.gz xproto.tar.gz
tar -xf xproto.tar.gz
cd xproto-7.0.31
./configure --prefix=${PREFIX}
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d xextproto-7.3.0 ]; then
download_file https://www.x.org/archive/individual/proto/xextproto-7.3.0.tar.gz xextproto.tar.gz
tar -xf xextproto.tar.gz
cd xextproto-7.3.0
./configure --prefix=${PREFIX}
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d kbproto-1.0.7 ]; then
download_file https://www.x.org/archive/individual/proto/kbproto-1.0.7.tar.gz kbproto.tar.gz
tar -xf kbproto.tar.gz
cd kbproto-1.0.7
./configure --prefix=${PREFIX}
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d inputproto-2.3.2 ]; then
download_file https://www.x.org/archive/individual/proto/inputproto-2.3.2.tar.gz inputproto.tar.gz
tar -xf inputproto.tar.gz
cd inputproto-2.3.2
./configure --prefix=${PREFIX}
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d freetype-2.13.3 ]; then
download_file http://downloads.sourceforge.net/project/freetype/freetype2/2.13.3/freetype-2.13.3.tar.xz freetype.tar.gz
tar -xf freetype.tar.gz
cd freetype-2.13.3
find . \( -iname '*.h.in' -o -iname '*.h' -o -iname '*.c' -o -iname '*.cc' -o -iname '*.cpp' -o -iname '*.cxx' \) | xargs sed -i '1s/^/#include "mp_embed.h"\n\'$'\n/g'
./configure --prefix=${PREFIX} --disable-shared --with-brotli=no
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d fontconfig-2.15.0 ]; then
download_file https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.15.0.tar.gz fontconfig.tar.gz
tar -xf fontconfig.tar.gz
cd fontconfig-2.15.0
find . \( -iname '*.h.in' -o -iname '*.h' -o -iname '*.c' -o -iname '*.cc' -o -iname '*.cpp' -o -iname '*.cxx' \) | xargs sed -i '1s/^/#include "mp_embed.h"\n\'$'\n/g'
./configure --prefix=${PREFIX} --disable-shared
make CPPFLAGS="-I${PREFIX}/include -I${PYTHON_BASE}/Include -DBYPASS_MP_EMBED" -j$(nproc --all) install || true
echo ----- It is normal for fontconfig to fail to build the executables. The libs should be enough. -----
cd ..
fi

if [ ! -d xcb-proto-1.16.0 ]; then
download_file https://xorg.freedesktop.org/archive/individual/proto/xcb-proto-1.16.0.tar.gz xcb-proto.tar.gz
tar -xf xcb-proto.tar.gz
cd xcb-proto-1.16.0
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d libXau-1.0.12 ]; then
download_file https://xorg.freedesktop.org/releases/individual/lib/libXau-1.0.12.tar.gz libXau.tar.gz
tar -xf libXau.tar.gz
cd libXau-1.0.12
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d libxcb-1.16 ]; then
download_file https://xorg.freedesktop.org/releases/individual/lib/libxcb-1.16.tar.gz libxcb.tar.gz
tar -xf libxcb.tar.gz
cd libxcb-1.16
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d libX11-1.8.7 ]; then
download_file https://xorg.freedesktop.org/releases/individual/lib/libX11-1.8.7.tar.gz libX11.tar.gz
tar -xf libX11.tar.gz
cd libX11-1.8.7
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d libXext-1.3.5 ]; then
download_file https://xorg.freedesktop.org/releases/individual/lib/libXext-1.3.5.tar.gz libXext.tar.gz
tar -xf libXext.tar.gz
cd libXext-1.3.5
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d scrnsaverproto-1.2.2 ]; then
download_file https://www.x.org/archive/individual/proto/scrnsaverproto-1.2.2.tar.gz scrnsaverproto.tar.gz
tar -xf scrnsaverproto.tar.gz
cd scrnsaverproto-1.2.2
./configure --prefix=${PREFIX}
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d randrproto-1.5.0 ]; then
download_file https://www.x.org/archive/individual/proto/randrproto-1.5.0.tar.gz randrproto.tar.gz
tar -xf randrproto.tar.gz
cd randrproto-1.5.0
./configure --prefix=${PREFIX}
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d renderproto-0.11.1 ]; then
download_file https://www.x.org/archive/individual/proto/renderproto-0.11.1.tar.gz renderproto.tar.gz
tar -xf renderproto.tar.gz
cd renderproto-0.11.1
./configure --prefix=${PREFIX}
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d libXScrnSaver-1.2.4 ]; then
download_file https://xorg.freedesktop.org/releases/individual/lib/libXScrnSaver-1.2.4.tar.gz libXScrnSaver.tar.gz
tar -xf libXScrnSaver.tar.gz
cd libXScrnSaver-1.2.4
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d libXdmcp-1.1.4 ]; then
download_file https://xorg.freedesktop.org/releases/individual/lib/libXdmcp-1.1.4.tar.gz libXdmcp.tar.gz
tar -xf libXdmcp.tar.gz
cd libXdmcp-1.1.4
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d libXrender-0.9.11 ]; then
download_file https://xorg.freedesktop.org/releases/individual/lib/libXrender-0.9.11.tar.gz libXrender.tar.gz
tar -xf libXrender.tar.gz
cd libXrender-0.9.11
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d libXrandr-1.5.4 ]; then
download_file https://xorg.freedesktop.org/releases/individual/lib/libXrandr-1.5.4.tar.gz libXrandr.tar.gz
tar -xf libXrandr.tar.gz
cd libXrandr-1.5.4
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d libXft-2.3.8 ]; then
download_file https://xorg.freedesktop.org/releases/individual/lib/libXft-2.3.8.tar.gz libXft.tar.gz
tar -xf libXft.tar.gz
cd libXft-2.3.8
find . \( -iname '*.h.in' -o -iname '*.h' -o -iname '*.c' -o -iname '*.cc' -o -iname '*.cpp' -o -iname '*.cxx' \) | xargs sed -i '1s/^/#include "mp_embed.h"\n\'$'\n/g'
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
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
make -j$(nproc --all)
make install
cd ../..
fi

if [ ! -d expat-2.5.0 ]; then
curl -L https://github.com/libexpat/libexpat/releases/download/R_2_5_0/expat-2.5.0.tar.gz -o expat.tar.gz
tar -xf expat.tar.gz
cd expat-2.5.0
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d tk8.6.15 ]; then
download_file http://downloads.sourceforge.net/project/tcl/Tcl/8.6.15/tk8.6.15-src.tar.gz tk.tar.gz
tar -xf tk.tar.gz
cd tk8.6.15/unix
./configure --prefix=${PREFIX} --enable-shared=no --enable-threads --with-tcl=${PREFIX}/lib
make -j$(nproc --all) "X11_LIB_SWITCHES=-l:libX11.a -l:libxcb.a -l:libXss.a -l:libfontconfig.a -l:libXft.a -l:libXext.a -l:libXrandr.a -l:libXau.a -l:libXrender.a -l:libXdmcp.a -l:libfreetype.a -l:libexpat.a -l:libpng.a -l:libharfbuzz.a -l:libX11.a -l:libxcb.a -l:libbz2.a"
make install
cd ../..
fi

if [ ! -d mpdecimal-4.0.0 ]; then
download_file https://www.bytereef.org/software/mpdecimal/releases/mpdecimal-4.0.0.tar.gz mpdecimal.tar.gz
tar -xf mpdecimal.tar.gz
cd mpdecimal-4.0.0
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

if [ ! -d libb2-0.98.1 ]; then
download_file https://github.com/BLAKE2/libb2/releases/download/v0.98.1/libb2-0.98.1.tar.gz libb2.tar.gz
tar -xf libb2.tar.gz
cd libb2-0.98.1
./configure --prefix=${PREFIX} --disable-shared
make -j$(nproc --all)
make install
cd ..
fi

cd ..

long_version=$(git branch --show-current 2>/dev/null || git symbolic-ref --short HEAD)
long_version=$(echo $long_version | sed -e 's#-.*##')
short_version=$(echo $long_version | sed -e 's#\.##')

target=/opt/monolithpy${short_version}

if [ ! -z "$1" ]
then
  target="$1"
fi

export "CFLAGS=-I${PREFIX}/include -fPIC -flto -fuse-linker-plugin -fno-fat-lto-objects"
export "CXXFLAGS=-I${PREFIX}/include -fPIC -flto -fuse-linker-plugin -fno-fat-lto-objects"
export "CPPFLAGS=-I${PREFIX}/include"

# The UCS4 has best compatibility with wheels on PyPI it seems.
./configure "--prefix=$target" --disable-shared --enable-ipv6 --enable-unicode=ucs4 \
  --enable-optimizations --with-lto --with-computed-gotos --with-fpectl --without-readline \
  --with-system-expat --with-system-libmpdec \
  CC="$CC" \
  CXX="$CXX" \
  CFLAGS="-g $CFLAGS" \
  LDFLAGS="-g -Xlinker -export-dynamic -rdynamic -Bsymbolic-functions -Wl,-z,relro -Wl,-allow-multiple-definition $LDFLAGS" \
  LIBS="-l:libffi.a -l:libbz2.a -l:libuuid.a -l:libsqlite3.a -l:liblzma.a -l:librt.a -l:libmp_embed.a" \
  PROFILE_TASK='-m test --pgo -x test_json ' \
  MODULE_BUILDTYPE=static \
  ax_cv_c_float_words_bigendian=no \
  ac_cv_lib_sqlite3_sqlite3_bind_double=yes ac_cv_lib_sqlite3_sqlite3_column_decltype=yes ac_cv_lib_sqlite3_sqlite3_column_double=yes \
  ac_cv_lib_sqlite3_sqlite3_complete=yes ac_cv_lib_sqlite3_sqlite3_enable_shared_cache=yes ac_cv_lib_sqlite3_sqlite3_progress_handler=yes \
  ac_cv_lib_sqlite3_sqlite3_result_double=yes ac_cv_lib_sqlite3_sqlite3_set_authorizer=yes ac_cv_lib_sqlite3_sqlite3_trace_v2=yes \
  ac_cv_lib_sqlite3_sqlite3_value_double=yes ac_cv_lib_sqlite3_sqlite3_load_extension=yes ac_cv_lib_sqlite3_sqlite3_serialize=yes \
  ___ORIG_DEPS_PREFIX=${PREFIX}___

make -j $(nproc --all) \
        EXTRA_CFLAGS="-flto -fuse-linker-plugin -fno-fat-lto-objects" \
        profile-opt

# Delayed deletion of old installation, to avoid having it not there for testing purposes
# while compiling, which is slow due to PGO beign applied.
$ELEVATE rm -rf "$target" && $ELEVATE make libinstall install

rm pybuilddir.txt

# Some things use the existence of this folder as an anchor so lets make sure it exists.
$ELEVATE mkdir -p "$target/lib/python${long_version}/lib-dynload"
$ELEVATE touch "$target/lib/python${long_version}/lib-dynload/.empty"

# Make sure to have pip installed, might even remove it afterwards, Debian
# e.g. doesn't include it.
$ELEVATE mv "$target/lib/python${long_version}/pip.py" "$target/lib/python${long_version}/pip.py.bak" && \
    $ELEVATE "$target/bin/python${long_version}" -m ensurepip && \
    $ELEVATE mv "$target/lib/python${long_version}/pip.py.bak" "$target/lib/python${long_version}/pip.py"

# Copy embedded data
$ELEVATE mv ${PREFIX}/lib/libmp_embed.a "$target/lib/libmp_embed.a"

$ELEVATE cp -v Modules/_hacl/libHacl_Hash_SHA2.a "$target/lib/"

$ELEVATE mkdir -p "$target/Embedded"
# The object file usually gets deleted during the build, so make sure to recompile here just in case.
rm -f Embedded/mp_embed.o
$CC -c -g -o Embedded/mp_embed.o Embedded/mp_embed.c -IInclude
$ELEVATE cp -r "Embedded/mp_embed.o" "$target/Embedded/"
$ELEVATE cp -r "Embedded/embed_data" "$target/Embedded/"

$ELEVATE mkdir -p "$target/dependency_libs"
$ELEVATE cp -r "$(pwd)/../MonolithPy-Deps" "$target/dependency_libs/base"
$ELEVATE ln -s base "$target/dependency_libs/bzip2"
$ELEVATE ln -s base "$target/dependency_libs/editline"
$ELEVATE ln -s base "$target/dependency_libs/expat"
$ELEVATE ln -s base "$target/dependency_libs/fontconfig"
$ELEVATE ln -s base "$target/dependency_libs/harfbuzz"
$ELEVATE ln -s base "$target/dependency_libs/b2"
$ELEVATE ln -s base "$target/dependency_libs/ffi"
$ELEVATE ln -s base "$target/dependency_libs/mpdecimal"
$ELEVATE ln -s base "$target/dependency_libs/png"
$ELEVATE ln -s base "$target/dependency_libs/X11"
$ELEVATE ln -s base "$target/dependency_libs/xcb"
$ELEVATE ln -s base "$target/dependency_libs/xcrypt"
$ELEVATE ln -s base "$target/dependency_libs/ncurses"
$ELEVATE ln -s base "$target/dependency_libs/openssl"
$ELEVATE ln -s base "$target/dependency_libs/sqlite"
$ELEVATE ln -s base "$target/dependency_libs/tcltk"
$ELEVATE ln -s base "$target/dependency_libs/uuid"
$ELEVATE ln -s base "$target/dependency_libs/xtrans"
$ELEVATE ln -s base "$target/dependency_libs/xz"
$ELEVATE ln -s base "$target/dependency_libs/zlib"

find "$target" \( -iname '*.la' -o -iname '*.pc' -o -iname '__pycache__' -o -iname 'link.json' \) | xargs $ELEVATE rm -rf

$ELEVATE "$target/bin/python${long_version}" -m rebuildpython
