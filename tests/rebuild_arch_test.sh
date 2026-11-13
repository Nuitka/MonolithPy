#!/bin/bash
# Interpreter rebuild test (macOS): rebuild a copy of a built MonolithPy with
# `python -m rebuildpython` while clang always runs as the OTHER architecture,
# and check the new interpreter still has the build's own architecture.
#
# The rebuild (what pip runs after installing a package with native code)
# compiles python.c itself. Without an explicit -arch, clang targets the
# architecture clang runs as, which need not be the interpreter's: an x86_64
# interpreter under Rosetta can start a clang that runs natively as arm64.
# Forcing clang to the other architecture makes that happen on any Apple
# Silicon machine.
#
# usage: tests/rebuild_arch_test.sh <MonolithPy install dir> <arm64|x86_64>
set -euo pipefail

src=$1
want=$2
case "$want" in
  arm64) other=x86_64 ;;
  x86_64) other=arm64 ;;
  *) echo "architecture must be arm64 or x86_64, not '$want'" >&2; exit 2 ;;
esac

work=$(mktemp -d "${TMPDIR:-/tmp}/rebuild-arch.XXXXXX")
trap 'rm -rf "$work"' EXIT

mkdir "$work/clang-$other"
for c in clang clang++; do
  printf '#!/bin/sh\nexec arch -%s /usr/bin/%s "$@"\n' "$other" "$c" > "$work/clang-$other/$c"
  chmod +x "$work/clang-$other/$c"
done
export PATH="$work/clang-$other:$PATH"
runs_as=$(clang -dumpmachine | cut -d- -f1)
if [ "$runs_as" != "$other" ]; then
  if [ "$other" = x86_64 ]; then
    # This toolchain has no x86_64 clang, so an arm64 interpreter can't meet one.
    echo "::notice::clang can't run as x86_64 here; skipping the rebuild test for $want"
    exit 0
  fi
  echo "clang wrapper runs as $runs_as, not $other" >&2
  exit 1
fi
echo "clang runs as $runs_as; rebuilding a $want interpreter"

cp -R "$src" "$work/mp"
py=$(find "$work/mp/bin" -maxdepth 1 -type f -name 'python3.*' | grep -E '/python3\.[0-9]+$' | head -1)
[ -n "$py" ] || { echo "no python3.N in $src/bin" >&2; exit 1; }

cd "$work"
start=$SECONDS
"$py" -m rebuildpython
archs=$(lipo -archs "$py")
[ "$archs" = "$want" ] || { echo "rebuilt interpreter is '$archs', expected $want" >&2; exit 1; }
"$py" -c 'import sys; print("rebuilt", sys.platform, sys.version.split()[0])'
echo "rebuild test passed in $((SECONDS - start)) s"
