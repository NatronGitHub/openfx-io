#!/usr/bin/env bash
# Compare two OFX plug-in bundles, e.g. the IO.ofx.bundle built by the Makefiles and the one
# installed by CMake, on Linux or macOS.
#
#   tools/compare_bundles.sh [-k] [-o OUTDIR] BUNDLE_A BUNDLE_B
#
# Compares: the file list (relative paths, types and link targets), Info.plist,
# Contents/Resources (sha256), the binary's file mode, its exported symbols, its dynamic
# section (Linux: readelf -d NEEDED, RPATH/RUNPATH, SONAME; macOS: otool -L, LC_RPATH), and
# the plug-ins each binary loads with RTLD_NOW and no LD_LIBRARY_PATH/DYLD_* variables
# (verify_plugin_loads, built from openfx's HostSupport as the CI load check does).
# Also checks each bundle on its own for libraries it can't find without those variables
# (Linux: ldd "not found"; macOS: absolute references outside the bundle and the system).
#
# Exit status: 0 if the bundles are equivalent and self-contained, 1 if they differ or a
# bundle has unresolved/outside references, 2 on a usage error, a missing tool or a failed
# check (the result can't be trusted).
#   -k        keep going when a tool is missing (the checks that need it are SKIPPED, exit 2)
#   -o DIR    where to write each check's raw output (default: a new temporary directory)
# CXX: the C++ compiler for the load checker (default c++; may include flags, e.g. "clang++ -std=c++17").
# The load checker needs the expat headers (openfx HostSupport), e.g. expat-devel or libexpat1-dev.

# the check functions are called indirectly, by compare and problems
# shellcheck disable=SC2329
set -u
usage() { sed -n '2,21p' "$0" | sed -e 's/^# \{0,1\}//'; exit 2; }
keep=0
outdir=""
while getopts "ko:" opt; do
    case $opt in
    k) keep=1 ;;
    o) outdir=$OPTARG ;;
    *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 2 ] && [ -d "$1" ] && [ -d "$2" ] || usage
A=$(cd "$1" && pwd -P)
B=$(cd "$2" && pwd -P)
srcdir=$(cd "$(dirname "$0")/.." && pwd)
[ -n "$outdir" ] || outdir=$(mktemp -d "${TMPDIR:-/tmp}/compare_bundles.XXXXXX")
mkdir -p "$outdir"
os=$(uname -s)
differ=0   # the bundles differ, or one of them isn't self-contained
broken=0   # a tool is missing or a check failed

# need TOOL...: check that the tools exist; with -k, return 1 (the caller skips its checks)
need() {
    local t
    for t in "$@"; do
        command -v "$t" >/dev/null 2>&1 && continue
        echo "  missing tool: $t"
        broken=1
        [ $keep = 1 ] || exit 2
        return 1
    done
    return 0
}

# the binary of a bundle: Contents/<archdir>/<name>.ofx
binary_of() {
    find "$1/Contents" -mindepth 2 -maxdepth 2 -type f -name '*.ofx' ! -path '*/Resources/*' | head -1
}

# compare NAME FUNCTION: run FUNCTION on each bundle (args: bundle, binary), with bash's pipefail so that
# a failing command is noticed; each function must succeed (exit 0) when it ran correctly. The bundle
# paths are replaced by <BUNDLE> so that only real differences show.
compare() {
    local name=$1 fn=$2 side bundle bin st stA=0 stB=0
    for side in A B; do
        if [ $side = A ]; then bundle=$A bin=$binA; else bundle=$B bin=$binB; fi
        (set -o pipefail; "$fn" "$bundle" "$bin") > "$outdir/$name.$side.raw" 2>&1
        st=$?
        sed -e "s#$bundle#<BUNDLE>#g" "$outdir/$name.$side.raw" > "$outdir/$name.$side.txt"
        if [ $side = A ]; then stA=$st; else stB=$st; fi
    done
    if [ $stA != 0 ] || [ $stB != 0 ]; then
        printf '  %-22s ERROR (exit A=%s B=%s; see %s/%s.[AB].txt)\n' "$name" "$stA" "$stB" "$outdir" "$name"
        sed -e 's/^/      /' "$outdir/$name.A.txt" "$outdir/$name.B.txt" | head -20
        broken=1
    elif diff -u "$outdir/$name.A.txt" "$outdir/$name.B.txt" > "$outdir/$name.diff"; then
        printf '  %-22s same\n' "$name"
    else
        printf '  %-22s DIFFERENT (%s)\n' "$name" "$outdir/$name.diff"
        sed -e 's/^/      /' "$outdir/$name.diff" | head -40
        differ=1
    fi
}

# problems NAME FUNCTION: run FUNCTION on each bundle; any output is a problem of that bundle
problems() {
    local name=$1 fn=$2 side bundle bin st
    for side in A B; do
        if [ $side = A ]; then bundle=$A bin=$binA; else bundle=$B bin=$binB; fi
        (set -o pipefail; "$fn" "$bundle" "$bin") > "$outdir/$name.$side.txt" 2>&1
        st=$?
        if [ $st != 0 ]; then
            printf '  %-22s ERROR on %s (exit %s)\n' "$name" "$side" "$st"
            sed -e 's/^/      /' "$outdir/$name.$side.txt" | head -10
            broken=1
        elif [ -s "$outdir/$name.$side.txt" ]; then
            printf '  %-22s %s: PROBLEM\n' "$name" "$side"
            sed -e 's/^/      /' "$outdir/$name.$side.txt" | head -20
            differ=1
        else
            printf '  %-22s %s: none\n' "$name" "$side"
        fi
    done
}

binA=$(binary_of "$A")
binB=$(binary_of "$B")
echo "A: $A"
echo "   binary: ${binA#"$A"/}"
echo "B: $B"
echo "   binary: ${binB#"$B"/}"
echo "report files: $outdir"
if [ -z "$binA" ] || [ -z "$binB" ]; then
    echo "error: no Contents/<arch>/*.ofx binary in one of the bundles"
    exit 2
fi

# sha256 of files: shasum (macOS, perl) or sha256sum (coreutils)
if command -v sha256sum >/dev/null 2>&1; then sha256() { sha256sum "$@"; }; elif command -v shasum >/dev/null 2>&1; then sha256() { shasum -a 256 "$@"; }; else sha256() { echo "no sha256sum or shasum" >&2; return 1; }; fi
# file mode: GNU or BSD stat
if stat --version >/dev/null 2>&1; then mode_of() { stat -c '%a' "$1"; }; else mode_of() { stat -f '%Lp' "$1"; }; fi

files() { # relative path, type and link target of every file, including the macOS signature seal
    (cd "$1" && find . -print | LC_ALL=C sort | while IFS= read -r f; do
        if [ -L "$f" ]; then echo "link $f -> $(readlink "$f")"; elif [ -d "$f" ]; then echo "dir $f"; else echo "file $f"; fi
    done)
}
plist() { cat "$1/Contents/Info.plist"; }
resources() {
    [ -d "$1/Contents/Resources" ] || { echo "(no Resources)"; return 0; }
    (cd "$1/Contents" && find Resources -type f | LC_ALL=C sort | while IFS= read -r f; do sha256 "$f"; done)
}
mode() { mode_of "$2"; }

echo
echo "== bundle contents"
compare files files
compare Info.plist plist
compare Resources resources
compare binary-mode mode

echo
echo "== binary"
if [ "$os" = Darwin ]; then
    if need nm otool; then
        exports() { nm -gU "$2" | awk '{print $NF}' | LC_ALL=C sort; }
        libs() { otool -L "$2" | tail -n +2 | awk '{print $1}' | LC_ALL=C sort; }
        rpaths() { otool -l "$2" | awk '/LC_RPATH/{getline; getline; print $2}'; }
        # absolute library references outside the bundle and the system, in every Mach-O file of the bundle
        outside() {
            local f
            find "$1/Contents" -type f -print | LC_ALL=C sort | while IFS= read -r f; do
                file -b "$f" | grep -q Mach-O || continue
                otool -L "$f" | tail -n +2 | awk '{print $1}' | grep -E '^/' | grep -vE '^/(usr/lib|System)/' || true
            done | LC_ALL=C sort -u
        }
        compare exported-symbols exports
        compare linked-libraries libs
        compare rpath rpaths
        echo
        echo "== self-containment (each bundle on its own)"
        problems outside-references outside
    else
        echo "  SKIPPED: exported symbols, linked libraries, rpath, outside references"
    fi
else
    if need readelf ldd; then
        exports() {
            if command -v nm >/dev/null 2>&1; then nm -D --defined-only "$2" | awk 'NF >= 3 {print $NF}'; else readelf --dyn-syms -W "$2" | awk '$7 != "UND" && $5 == "GLOBAL" {print $8}'; fi | LC_ALL=C sort -u
        }
        needed() { readelf -d "$2" | awk '/\(NEEDED\)/{print $NF}' | tr -d '[]' | LC_ALL=C sort; }
        rpaths() { readelf -d "$2" | awk '/\((RPATH|RUNPATH)\)/{print $2, $NF}' | tr -d '[]'; }
        soname() { readelf -d "$2" | awk '/\(SONAME\)/{print $NF}' | tr -d '[]'; }
        unresolved() { env -u LD_LIBRARY_PATH ldd "$2" | { grep -E 'not found' || true; } | awk '{print $1}' | LC_ALL=C sort; }
        compare exported-symbols exports
        compare needed-libraries needed
        compare rpath rpaths
        compare soname soname
        echo
        echo "== self-containment (each bundle on its own)"
        problems unresolved-libraries unresolved
    else
        echo "  SKIPPED: exported symbols, dynamic section, unresolved libraries"
    fi
fi

echo
echo "== plug-ins loaded (RTLD_NOW, no LD_LIBRARY_PATH/DYLD_*)"
read -r -a cxx <<< "${CXX:-c++}"
if need "${cxx[0]}"; then
    verify="$outdir/verify_plugin_loads"
    ldl=""
    [ "$os" = Darwin ] || ldl="-ldl"
    sed -e 's/RTLD_LAZY/RTLD_NOW/g' "$srcdir/openfx/HostSupport/src/ofxhBinary.cpp" > "$outdir/ofxhBinaryStrict.cpp"
    # as in the CI load checks: ofxhBinary.cpp only (Linux, macOS), plus ofxhUtilities.cpp if needed
    if "${cxx[@]}" -std=c++17 -o "$verify" "$srcdir/.github/workflows/verify_plugin_loads.cpp" "$outdir/ofxhBinaryStrict.cpp" \
        -I "$srcdir/openfx/HostSupport/include" -I "$srcdir/openfx/include" $ldl > "$outdir/verify-build.log" 2>&1 ||
        "${cxx[@]}" -std=c++17 -o "$verify" "$srcdir/.github/workflows/verify_plugin_loads.cpp" "$outdir/ofxhBinaryStrict.cpp" \
        "$srcdir/openfx/HostSupport/src/ofxhUtilities.cpp" -I "$srcdir/openfx/HostSupport/include" -I "$srcdir/openfx/include" $ldl \
        >> "$outdir/verify-build.log" 2>&1; then
        # the checker's output and exit status are compared: a bundle that fails to load differs from one that loads
        load() { env -u LD_LIBRARY_PATH -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH "$verify" "$2" 2>&1 | grep -v '^$'; echo "exit status: ${PIPESTATUS[0]}"; }
        compare plug-ins load
        grep -h -E "Num_plugins|exit status" "$outdir/plug-ins.A.txt" "$outdir/plug-ins.B.txt" | sed -e 's/^/      /'
        if ! grep -q "^exit status: 0$" "$outdir/plug-ins.A.txt" || ! grep -q "^exit status: 0$" "$outdir/plug-ins.B.txt"; then
            echo "  a bundle failed to load"
            differ=1
        fi
    else
        echo "  ERROR: could not build the load checker (see $outdir/verify-build.log)"
        sed -e 's/^/      /' "$outdir/verify-build.log" | head -10
        broken=1
    fi
else
    echo "  SKIPPED: plug-in load check"
fi

echo
if [ $broken = 1 ]; then
    echo "RESULT: INCOMPLETE: a tool is missing or a check failed (see above); the comparison can't be trusted"
    exit 2
elif [ $differ = 0 ]; then
    echo "RESULT: equivalent"
    exit 0
else
    echo "RESULT: different (details above and in $outdir)"
    exit 1
fi
