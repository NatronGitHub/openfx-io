#!/bin/bash
# Copy the licences of the libraries of a testing bundle into it (#272), one directory per package:
#
#   copy_bundled_licenses.sh <dependency JSON> <binary directory of the bundle> <licence directory>
#
# <dependency JSON>: the --json output of find_and_copy_deps.py, whose binary_locations give where each bundled library
# was copied from. For each file of the binary directory except the plug-in (*.ofx), the package that owns that file:
# - dpkg (Ubuntu, Debian): <licence directory>/<package>/copyright, the package's Debian copyright file
#   (/usr/share/doc/<package>/copyright). The licence texts of /usr/share/common-licenses that these files refer to
#   instead of quoting them (GPL-2, LGPL-2.1, Apache-2.0...) are copied once, into <licence directory>/common-licenses.
# - pacman (MSYS2): PACKAGE-INFO.txt (pacman -Qi: its version, URL and licence names), and the package's licence files
#   (<prefix>/share/licenses/...). Some packages have none: for those, the texts of the SPDX licence ids that pacman
#   reports ("spdx:..." entries, and the legacy names of LEGACY_SPDX) are copied once, from SPDX's licence list
#   (SPDX_LICENSE_LIST), into <licence directory>/spdx/<id>.txt, and PACKAGE-INFO.txt ends with a
#   "Licence texts (SPDX):" line that lists them (" none" for "custom" licences).
# - none (a library that CI builds, e.g. FFmpeg or SeExpr, in <prefix>/lib): the only directory of
#   <prefix>/share/licenses, into which the CI step that builds the library copies its licence files.
# <licence directory>/LIBRARIES.txt lists each library, its package and the package's version ("-" for the libraries
# that CI builds). check_bundled_licenses.sh checks the result.

set -euo pipefail

if [ $# -ne 3 ]; then
  echo "usage: $0 <dependency JSON> <binary directory of the bundle> <licence directory>" >&2
  exit 2
fi
json=$1
bindir=$2
licdir=$3
# the licence texts of https://github.com/spdx/license-list-data, at a pinned release
SPDX_LICENSE_LIST=v3.29.0
COMMON_LICENSES=/usr/share/common-licenses

libs=()
for f in "$bindir"/*; do
  case "$f" in *.ofx) ;; *) libs+=("$(basename "$f")") ;; esac
done
test ${#libs[@]} -gt 0 || { echo "error: no library in $bindir" >&2; exit 1; }
# the source of each library, in the order of libs (MSYS2's python3 is a Windows program: drop the CRs)
mapfile -t srcs < <(python3 -c 'import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    locations = json.load(f)["binary_locations"]
for lib in sys.argv[2:]:
    print(locations.get(lib, ""))' "$json" "${libs[@]}" | tr -d '\r')
test ${#srcs[@]} = ${#libs[@]} || { echo "error: couldn't read $json" >&2; exit 1; }
for i in "${!libs[@]}"; do
  test -n "${srcs[$i]}" || { echo "error: ${libs[$i]} is bundled, but not in $json" >&2; exit 1; }
done
if command -v cygpath > /dev/null; then
  mapfile -t srcs < <(printf '%s\n' "${srcs[@]}" | cygpath -u -f -)
  test ${#srcs[@]} = ${#libs[@]} ||
    { echo "error: cygpath returned ${#srcs[@]} paths for ${#libs[@]} libraries" >&2; exit 1; }
fi
for i in "${!libs[@]}"; do
  cmp -s "${srcs[$i]}" "$bindir/${libs[$i]}" ||
    { echo "error: the bundled ${libs[$i]} is not ${srcs[$i]}" >&2; exit 1; }
done

# pacman: the package and version of each library that a package owns, from one pacman -Qo
# ("<file> is owned by <package> <version>" lines)
declare -A pacman_owner=()
if command -v pacman > /dev/null; then
  while read -r line; do
    file=${line% is owned by *}
    pacman_owner[$(basename "$file")]=${line##* is owned by }
  done < <(pacman -Qo "${srcs[@]}" 2> /dev/null | grep ' is owned by ' || true)
fi

# dpkg: prints the package (<name>[:<arch>]) that owns $1. usrmerge: dpkg may know /usr/lib/x as /lib/x.
# Returns 1 if no package owns it, 2 if several do.
dpkg_owner() {
  local p q out
  p=$(readlink -f "$1")
  for q in "$p" "${p#/usr}"; do
    out=$(dpkg -S "$q" 2> /dev/null | grep -v '^diversion by ' || true)
    test -n "$out" || continue
    # "<package>[:<arch>]: <file>", or "<package>, <package>: <file>"
    if [ "$(wc -l <<< "$out")" -ne 1 ] || [[ "${out%%: *}" == *", "* ]]; then
      echo "error: several packages own $q: $out" >&2
      return 2
    fi
    echo "${out%%: *}"
    return 0
  done
  return 1
}

# the SPDX ids of the licences in pacman -Qi's output $1, one per line. pacman separates the licences with two spaces:
# "spdx:<expression>" ones, and legacy names, which LEGACY_SPDX maps (as the packages of NO_LICENCE_FILE_PACKAGES in
# check_bundled_licenses.sh use them: gmp "LGPL3 GPL" is LGPL-3.0-or-later or GPL-2.0-or-later, lame "LGPL" is
# LGPL-2.0-or-later, x265 and xvidcore "GPL" are GPL-2.0-or-later, opencore-amr "Apache" is Apache-2.0). "custom"
# (libvorbis, libx264) has no SPDX text.
declare -A LEGACY_SPDX=([Apache]=Apache-2.0 [LGPL3]=LGPL-3.0-or-later [LGPL]=LGPL-2.0-or-later [GPL]=GPL-2.0-or-later)
licence_ids() {
  local entry
  sed -n 's/^Licenses *: //p' <<< "$1" | awk -F'  ' '{for (i = 1; i <= NF; i++) print $i}' |
    while IFS= read -r entry; do
      case "$entry" in
        spdx:*) grep -oE '[A-Za-z0-9.+-]+' <<< "${entry#spdx:}" | grep -vxE 'AND|OR|WITH' || true ;;
        *) if [ -n "${LEGACY_SPDX[$entry]:-}" ]; then echo "${LEGACY_SPDX[$entry]}"; fi ;;
      esac
    done | sort -u
}

mkdir -p "$licdir"
index=$licdir/LIBRARIES.txt
: > "$index"
for i in "${!libs[@]}"; do
  lib=${libs[$i]}
  src=${srcs[$i]}
  rc=0
  qual=
  if command -v dpkg > /dev/null; then
    qual=$(dpkg_owner "$src") || rc=$?
    test "$rc" != 2 || exit 1
  fi
  if [ -n "$qual" ]; then
    pkg=${qual%%:*}
    version=$(dpkg-query -W -f '${Version}\n' "$qual" | sort -u)
    test "$(wc -l <<< "$version")" -eq 1 || { echo "error: $qual has several versions: $version" >&2; exit 1; }
    if [ ! -d "$licdir/$pkg" ]; then
      test -f "/usr/share/doc/$pkg/copyright" ||
        { echo "error: /usr/share/doc/$pkg/copyright not found ($lib)" >&2; exit 1; }
      mkdir "$licdir/$pkg"
      cp "/usr/share/doc/$pkg/copyright" "$licdir/$pkg/"
    fi
  elif [ -n "${pacman_owner[$lib]:-}" ]; then
    read -r pkg version <<< "${pacman_owner[$lib]}"
    if [ ! -d "$licdir/$pkg" ]; then
      mkdir "$licdir/$pkg"
      info=$(pacman -Qi "$pkg")
      echo "$info" > "$licdir/$pkg/PACKAGE-INFO.txt"
      files=$(pacman -Qlq "$pkg" | grep '/share/licenses/.*[^/]$' || true)
      if [ -n "$files" ]; then
        # keep their paths below share/licenses (e.g. <name>/COPYING)
        while IFS= read -r l; do
          rel=${l#*/share/licenses/}
          mkdir -p "$licdir/$pkg/$(dirname "$rel")"
          cp "$l" "$licdir/$pkg/$rel"
        done <<< "$files"
      else
        ids=$(licence_ids "$info")
        echo "::warning::MSYS2 package $pkg ($lib) has no licence file: PACKAGE-INFO.txt, and the SPDX texts of:" \
          "${ids:-(none)}"
        texts=
        for id in $ids; do
          texts+=" spdx/$id.txt"
          test -f "$licdir/spdx/$id.txt" && continue
          mkdir -p "$licdir/spdx"
          curl -fsSL --retry 3 -o "$licdir/spdx/$id.txt" \
            "https://raw.githubusercontent.com/spdx/license-list-data/$SPDX_LICENSE_LIST/text/$id.txt"
        done
        # read by check_bundled_licenses.sh
        echo "Licence texts (SPDX):${texts:- none}" >> "$licdir/$pkg/PACKAGE-INFO.txt"
      fi
    fi
  else
    version=-
    prefix=$(dirname "$(dirname "$src")")
    dirs=("$prefix"/share/licenses/*/)
    if [ ${#dirs[@]} != 1 ] || [ ! -d "${dirs[0]}" ]; then
      echo "error: $lib ($src) is in no package, and $prefix/share/licenses doesn't have exactly one directory" >&2
      exit 1
    fi
    pkg=$(basename "${dirs[0]}")
    if [ ! -d "$licdir/$pkg" ]; then
      mkdir "$licdir/$pkg"
      cp -R "${dirs[0]}." "$licdir/$pkg/"
    fi
  fi
  echo "$lib $pkg $version" >> "$index"
  echo "$lib: $src, from $pkg $version"
done

# dpkg: the common licences that the Debian copyright files refer to, once: <dir>/<name>, or <dir>/{<name>,<name>...}
# (e.g. libffi8's)
if command -v dpkg > /dev/null; then
  names=$(find "$licdir" -name copyright -type f -exec grep -ohE \
    "$COMMON_LICENSES/(\{[A-Za-z0-9._,-]+\}|[A-Za-z0-9._-]*[A-Za-z0-9])" {} + |
    sed 's|.*/||; s/[{}]//g' | tr ',' '\n' | sort -u || true)
  for name in $names; do
    test -f "$COMMON_LICENSES/$name" ||
      { echo "error: $COMMON_LICENSES/$name, which a copyright file refers to, doesn't exist" >&2; exit 1; }
    mkdir -p "$licdir/common-licenses"
    cp "$COMMON_LICENSES/$name" "$licdir/common-licenses/"
  done
  echo "common licences: ${names//$'\n'/ }"
fi
