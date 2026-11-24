#!/bin/bash
# Check that every library of a testing bundle comes with its licence (#272): each file of the bundle's binary
# directory, except the plug-in (*.ofx), must be listed in <licence directory>/LIBRARIES.txt (lines
# "<library> <package> ...", written by copy_bundled_licenses.sh), and <licence directory>/<package> must contain a
# licence file (PACKAGE-INFO.txt, pacman's package information, doesn't count). The only exceptions are the MSYS2
# packages of NO_LICENCE_FILE_PACKAGES, which install no licence file: they have only their licence names
# (PACKAGE-INFO.txt) and, except for "custom" licences, the generic SPDX texts of these licences (the spdx/ files that
# the "Licence texts (SPDX):" line of PACKAGE-INFO.txt lists, which must exist). dpkg: each
# /usr/share/common-licenses/<name> (or {<name>,<name>...}) that a Debian copyright file refers to must be in
# <licence directory>/common-licenses.
#
#   check_bundled_licenses.sh <binary directory of the bundle> <licence directory>

set -euo pipefail

# The MSYS2 (UCRT64) packages that install no licence file in share/licenses, without their mingw-w64-ucrt-x86_64-
# prefix (checked against MSYS2's ucrt64.files database). A new package without a licence file fails the check: add it
# here only after checking that its licence is in PACKAGE-INFO.txt.
NO_LICENCE_FILE_PACKAGES="ffmpeg giflib gmp gnutls nettle libidn2 lz4 lame opencore-amr p11-kit ptex rtmpdump snappy
  libssh libtheora libvorbis libvpx libx264 x265 xvidcore"

if [ $# -ne 2 ]; then
  echo "usage: $0 <binary directory of the bundle> <licence directory>" >&2
  exit 2
fi
bindir=$1
licdir=$2
test -d "$bindir" || { echo "$bindir is not a directory" >&2; exit 2; }
index=$licdir/LIBRARIES.txt
known=" ${NO_LICENCE_FILE_PACKAGES//$'\n'/ } "

fail=0
n=0
names_only=""
no_text=""
for f in "$bindir"/*; do
  lib=$(basename "$f")
  case "$lib" in *.ofx) continue ;; esac
  n=$((n + 1))
  pkg=$(awk -v l="$lib" '$1 == l {print $2; exit}' "$index" 2> /dev/null || true)
  if [ -z "$pkg" ]; then
    echo "error: $lib is bundled, but not in $index"
    fail=1
    continue
  fi
  base=${pkg#mingw-w64-*x86_64-}
  if [ -n "$(find "$licdir/$pkg" -type f ! -name PACKAGE-INFO.txt 2> /dev/null | head -1)" ]; then
    echo "  $lib: $pkg"
    if [[ "$known" == *" $base "* ]]; then
      echo "warning: $pkg has licence files now: remove $base from NO_LICENCE_FILE_PACKAGES in $0"
    fi
  elif [[ "$known" == *" $base "* ]] && [ -f "$licdir/$pkg/PACKAGE-INFO.txt" ]; then
    texts=$(sed -n 's/^Licence texts (SPDX)://p' "$licdir/$pkg/PACKAGE-INFO.txt")
    echo "  $lib: $pkg (licence names only, in PACKAGE-INFO.txt; texts:${texts:- missing})"
    if [ -z "$texts" ]; then
      echo "error: $licdir/$pkg/PACKAGE-INFO.txt has no \"Licence texts (SPDX):\" line"
      fail=1
    elif [ "$texts" = " none" ]; then
      [[ " $no_text " == *" $pkg "* ]] || no_text+=" $pkg"
    else
      for t in $texts; do
        test -f "$licdir/$t" || { echo "error: $licdir/$t (of $pkg) doesn't exist"; fail=1; }
      done
      [[ " $names_only " == *" $pkg "* ]] || names_only+=" $pkg"
    fi
  else
    echo "error: $lib is bundled (package $pkg), but $licdir/$pkg has no licence file"
    fail=1
  fi
done
test "$n" -gt 0 || { echo "error: no library in $bindir: nothing was checked" >&2; exit 1; }

# dpkg: the common licences that the Debian copyright files refer to
if command -v dpkg > /dev/null; then
  names=$(find "$licdir" -name copyright -type f -exec grep -ohE \
    '/usr/share/common-licenses/(\{[A-Za-z0-9._,-]+\}|[A-Za-z0-9._-]*[A-Za-z0-9])' {} + |
    sed 's|.*/||; s/[{}]//g' | tr ',' '\n' | sort -u || true)
  for name in $names; do
    if [ ! -f "$licdir/common-licenses/$name" ]; then
      echo "error: a copyright file refers to /usr/share/common-licenses/$name, which is not in $licdir/common-licenses"
      fail=1
    fi
  done
fi

if [ "$fail" != 0 ]; then
  echo "error: some of the $n bundled libraries have no licence (see copy_bundled_licenses.sh)"
  exit 1
fi
packages=$(awk '{print $2}' "$index" | sort -u | wc -l)
k=$(wc -w <<< "$names_only")
c=$(wc -w <<< "$no_text")
echo "== OK: the $n bundled libraries ($((packages)) packages) have their licences in $licdir"
echo "  $((k)) packages have only their licence names (PACKAGE-INFO.txt) and their generic SPDX texts" \
  "(spdx/):${names_only:- none}"
echo "  $((c)) packages have only their licence names (\"custom\": no licence text at all):${no_text:- none}"
