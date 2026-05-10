#!/bin/bash
set -ux
WORK=/tmp/mp_work
SRC=/mnt/c/src/workspace/Nuitka-Python
mkdir -p "$WORK/gen" "$WORK/tree"

# Build libzstd.a once if not already cached. mp_embed needs ZSTD_decompress
# and ZSTD_getFrameContentSize for the compressed-blob path. We download the
# tarball into a persistent /tmp dir so subsequent runs are fast.
ZSTD_DEP=/tmp/mp_zstd
ZSTD_DIR="$ZSTD_DEP/zstd-1.5.7"
ZSTD_INC="$ZSTD_DIR/lib"
ZSTD_AR="$WORK/libzstd.a"
if [ ! -f "$ZSTD_AR" ]; then
  mkdir -p "$ZSTD_DEP"
  if [ ! -d "$ZSTD_DIR" ]; then
    curl -L -o "$ZSTD_DEP/zstd.tar.gz" \
      https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    tar -xf "$ZSTD_DEP/zstd.tar.gz" -C "$ZSTD_DEP"
  fi
  make -C "$ZSTD_INC" -j$(nproc) libzstd.a >/dev/null
  cp "$ZSTD_INC/libzstd.a" "$ZSTD_AR"
fi
rm -rf "$WORK/test_mp_embed" "$WORK/test_mp_embed_cpp" \
       "$WORK/mp_embed.o" "$WORK/mp_embed_data.o" \
       "$WORK/gen" "$WORK/tree"
mkdir -p "$WORK/gen" "$WORK/tree"

# Generate test tree under /tmp (slow on /mnt/c, fast on /tmp).
python3 - <<PY
import os, sys
sys.path.insert(0, "$SRC/tests/mp_embed")
import build_test_tree as b
b.ROOT = "$WORK/tree"
b.main()
PY

# Build the embed blob (now zstd-compressed).
python3 "$SRC/Lib/mkembeddata.py" "$WORK/gen" "$WORK/tree"
ls "$WORK/gen"

# Build mp_embed.o once with the same flags build.sh uses for the
# production artifact (-DMP_EMBED_USE_WRAP). Both test suites link this
# single object plus the test-specific mp_embed_data.o, mirroring how
# CI links the artifact's mp_embed.o against the test data.
cc -O2 -DMP_EMBED_USE_WRAP -c -o "$WORK/mp_embed.o" "$SRC/Embedded/mp_embed.c" -I"$SRC/Include" -I"$SRC/PC" -I"$ZSTD_INC"
cc -O2 -c -o "$WORK/mp_embed_data.o" "$WORK/gen/mp_embed_data.c"

WRAPS="-Wl,--wrap=fopen,--wrap=fopen64,--wrap=fclose,--wrap=read,--wrap=lseek,--wrap=lseek64,--wrap=fstat,--wrap=fstat64,--wrap=close"

# C suite.
cc -O2 -pthread $WRAPS -o "$WORK/test_mp_embed" \
    "$SRC/tests/mp_embed/test_mp_embed.c" \
    "$WORK/mp_embed.o" "$WORK/mp_embed_data.o" "$ZSTD_AR" \
    -I"$SRC/Include" -I"$SRC/PC"
echo "=== C suite ==="
timeout 60 "$WORK/test_mp_embed" 2>&1 | tail -5 || echo "(C suite timed out or returned non-zero)"

# C++ suite. -static-libstdc++ + --wrap so libstdc++.a's basic_file
# entry points (fopen/fclose/read/lseek/fstat/close) route to our
# __wrap_* and serve from embed memory zero-copy.
c++ -std=c++17 -O2 -pthread -static-libstdc++ $WRAPS \
    -o "$WORK/test_mp_embed_cpp" \
    "$SRC/tests/mp_embed/test_mp_embed.cpp" \
    "$WORK/mp_embed.o" "$WORK/mp_embed_data.o" "$ZSTD_AR" \
    -I"$SRC/Include" -I"$SRC/PC"
echo "=== C++ suite ==="
timeout 60 "$WORK/test_mp_embed_cpp" 2>&1 | tail -5 || echo "(C++ suite timed out or returned non-zero)"
