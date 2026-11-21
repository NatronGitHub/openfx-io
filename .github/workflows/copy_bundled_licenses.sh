#!/bin/bash
# Copy the licences of some bundled Ubuntu libraries into a Linux testing bundle: the Debian copyright file
# (/usr/share/doc/<package>/copyright, which has the licence text) of the package that installed each one.
#
#   copy_bundled_licenses.sh <directory of the bundled libraries> <licence directory>
#
# Each library of LIBS that is in the bundle gets <licence directory>/<library>.<package>.copyright.
# LIBS: the OpenCL ICD loader (ocl-icd, BSD-2-Clause), bundled since #270.
# Requires dpkg (Ubuntu, Debian).

set -euo pipefail

if [ $# -ne 2 ]; then
  echo "usage: $0 <directory of the bundled libraries> <licence directory>" >&2
  exit 2
fi
libdir=$1
licdir=$2
LIBS="libOpenCL.so.1"

for lib in $LIBS; do
  [ -e "$libdir/$lib" ] || continue
  # the system copy that find_and_copy_deps.py bundled
  # (awk reads all its input: an early exit would make ldconfig fail with SIGPIPE, under pipefail)
  src=$(ldconfig -p | awk -v l="$lib" '!f && $1 == l && /x86-64/ {print $NF; f = 1}')
  test -n "$src" || { echo "error: $lib is bundled, but not in the system (ldconfig -p)" >&2; exit 1; }
  cmp -s "$src" "$libdir/$lib" || { echo "error: the bundled $lib is not $src" >&2; exit 1; }
  pkg=$(dpkg -S "$(readlink -f "$src")" | awk -F: '!f {print $1; f = 1}')
  copyright=/usr/share/doc/$pkg/copyright
  test -f "$copyright" || { echo "error: $copyright not found (package of $src: $pkg)" >&2; exit 1; }
  mkdir -p "$licdir"
  cp "$copyright" "$licdir/$lib.$pkg.copyright"
  echo "$lib: $src, from $pkg $(dpkg-query -W -f '${Version}' "$pkg"): $licdir/$lib.$pkg.copyright"
done
