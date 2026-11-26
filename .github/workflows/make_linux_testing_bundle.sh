#!/bin/bash
# Make a Linux testing bundle self-contained, and check it (the linux and linux-natron jobs of ci.yml):
#
#   make_linux_testing_bundle.sh <binary directory of the bundle> <search directories...>
#
# Run from the source directory, after the CMake build (build/CMakeCache.txt gives linuxCheckBundle.sh), with the
# OFX_ABI_* limits of linuxCheckBundle.sh and DESKTOP_LIBS_RE in the environment. <binary directory> is
# <dir>/IO.ofx.bundle/Contents/Linux-x86-64. Writes IO.manifest and bundle-deps.json in the current directory.
# 1. find_and_copy_deps.py copies the libraries of IO.ofx, found in the search directories, into the bundle, except the
#    system libraries (see find_and_copy_deps.py and "Linux runtime requirements" in README.md);
# 2. copy_bundled_licenses.sh copies their licences (their packages' Debian copyright files and the common licences
#    they refer to, and those of the libraries that CI builds) into Contents/Resources/Licenses;
# 3. IO.ofx gets the rpath $ORIGIN;
# 4. linuxCheckBundle.sh: no system library may be bundled (--no-needed: the copied libraries find their dependencies
#    in the system), and neither may the desktop libraries (X11..., DESKTOP_LIBS_RE);
# 5. check_bundled_licenses.sh: every bundled library comes with its licence (#272).

set -euo pipefail

if [ $# -lt 1 ]; then
  echo "usage: $0 <binary directory of the bundle> <search directories...>" >&2
  exit 2
fi
bindir=$1
shift
here=$(dirname "$0")
contents=$(dirname "$bindir")
licdir=$contents/Resources/Licenses
# the directory that contains IO.ofx.bundle
plugins=$(dirname "$(dirname "$contents")")

python3 "$here/find_and_copy_deps.py" --manifest=IO.manifest --json=bundle-deps.json "$bindir/IO.ofx" "$bindir" "$@"
bash "$here/copy_bundled_licenses.sh" bundle-deps.json "$bindir" "$licdir"
# shellcheck disable=SC2016 # $ORIGIN is for the dynamic loader
patchelf --force-rpath --set-rpath '$ORIGIN' "$bindir/IO.ofx"

# (grep || true: if nothing matches, the check below says so)
desktop=$(find /usr/lib/x86_64-linux-gnu -maxdepth 1 -printf '%f\n' | { grep -E "$DESKTOP_LIBS_RE" || true; } | sort | tr '\n' ' ')
[[ " $desktop " == *" libX11.so.6 "* && " $desktop " == *" libxcb.so.1 "* ]] ||
  { echo "no X11 libraries in DESKTOP_LIBS_RE's list: $desktop"; exit 1; }
check=$(sed -n 's/^OFX_LINUX_CHECK_BUNDLE:FILEPATH=//p' build/CMakeCache.txt)
test -n "$check" || { echo "OFX_LINUX_CHECK_BUNDLE is not in build/CMakeCache.txt"; exit 1; }
OFX_ABI_SYSTEM_LIBS="$desktop" "$check" -v --no-needed "$plugins"

bash "$here/check_bundled_licenses.sh" "$bindir" "$licdir"
