#!/usr/bin/env bash
# Compare two OFX plug-in bundles, e.g. the IO.ofx.bundle built by the Makefiles and the one
# installed by CMake, on Linux or macOS.
#
#   tools/compare_bundles.sh [-k] [-o OUTDIR] BUNDLE_A BUNDLE_B
#
# Compares: the file list (relative paths and types), Info.plist, Contents/Resources,
# the exported symbols of each binary, the dynamic section (Linux: readelf -d NEEDED,
# RPATH/RUNPATH, SONAME; macOS: otool -L, LC_RPATH), unresolved libraries (Linux: ldd;
# macOS: references outside the bundle), the file mode, and the plug-ins each binary
# loads (with RTLD_NOW, built from openfx's HostSupport as the CI load check does).
# The plug-in load check runs without LD_LIBRARY_PATH / DYLD_* variables, so it shows
# whether each bundle loads on its own.
#
# Prints a report and exits 1 if anything differs, 0 if the bundles are equivalent.
# Each comparison's raw output is kept in OUTDIR (default: a new temporary directory).
#   -k   keep going (don't stop at the first missing tool); OUTDIR is always kept.

set -u
keep=0
outdir=""
while getopts "ko:" opt; do
    case $opt in
    k) keep=1 ;;
    o) outdir=$OPTARG ;;
    *) sed -n '2,17p' "$0"; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
if [ $# -ne 2 ] || [ ! -d "$1" ] || [ ! -d "$2" ]; then
    sed -n '2,17p' "$0"
    exit 2
fi
A=$(cd "$1" && pwd)
B=$(cd "$2" && pwd)
srcdir=$(cd "$(dirname "$0")/.." && pwd)
[ -n "$outdir" ] || outdir=$(mktemp -d "${TMPDIR:-/tmp}/compare_bundles.XXXXXX")
mkdir -p "$outdir"
os=$(uname -s)
differ=0

need() {
    command -v "$1" >/dev/null 2>&1 && return 0
    echo "missing tool: $1"
    [ $keep = 1 ] || exit 2
    return 1
}

# the binary of a bundle: Contents/<archdir>/<name>.ofx
binary_of() {
    find "$1/Contents" -mindepth 2 -maxdepth 2 -type f -name '*.ofx' ! -path '*/Resources/*' | head -1
}

# compare the outputs of a command run on A and on B
compare() { # name, command (receives the bundle and its binary)
    local name=$1
    shift
    "$@" "$A" "$binA" > "$outdir/$name.A.txt" 2>&1
    "$@" "$B" "$binB" > "$outdir/$name.B.txt" 2>&1
    if diff -u "$outdir/$name.A.txt" "$outdir/$name.B.txt" > "$outdir/$name.diff"; then
        printf '  %-22s same\n' "$name"
    else
        printf '  %-22s DIFFERENT (%s)\n' "$name" "$outdir/$name.diff"
        sed -e 's/^/      /' "$outdir/$name.diff" | head -40
        differ=1
    fi
}

binA=$(binary_of "$A")
binB=$(binary_of "$B")
echo "A: $A"
echo "   binary: ${binA#$A/}"
echo "B: $B"
echo "   binary: ${binB#$B/}"
echo "report files: $outdir"
if [ -z "$binA" ] || [ -z "$binB" ]; then
    echo "error: no Contents/<arch>/*.ofx binary in one of the bundles"
    exit 2
fi

files() { # relative path and type of every file (the signature seal is reported separately)
    (cd "$1" && find . -print | LC_ALL=C sort | while read -r f; do
        if [ -L "$f" ]; then t="link"; elif [ -d "$f" ]; then t="dir"; else t="file"; fi
        echo "$t $f"
    done)
}
plist() { cat "$1/Contents/Info.plist"; }
resources() { (cd "$1/Contents" && [ -d Resources ] && find Resources -type f -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256 2>/dev/null || true); }
mode() { if [ "$os" = Darwin ]; then stat -f '%Lp' "$2"; else stat -c '%a' "$2"; fi; }

echo
echo "== bundle contents"
compare files files
compare Info.plist plist
compare Resources resources
compare binary-mode mode

echo
echo "== binary"
if [ "$os" = Darwin ]; then
    need nm && need otool
    exports() { nm -gU "$2" | awk '{print $NF}' | LC_ALL=C sort; }
    # library references, with the bundle's own path and versions removed
    libs() { otool -L "$2" | tail -n +2 | awk '{print $1}' | LC_ALL=C sort; }
    rpaths() { otool -l "$2" | awk '/LC_RPATH/{getline; getline; print $2}'; }
    outside() { # references to libraries outside the bundle and the system, in the binary and its libraries
        find "$1/Contents" -type f \( -name '*.ofx' -o -name '*.dylib' -o -name '*.so' \) -print0 |
            xargs -0 otool -L 2>/dev/null | grep -E '^[[:space:]]+/(opt|usr/local)/' | awk '{print $1}' | LC_ALL=C sort -u
    }
    compare exported-symbols exports
    compare linked-libraries libs
    compare rpath rpaths
    compare outside-refs outside
else
    need readelf && need ldd
    exports() {
        if command -v nm >/dev/null 2>&1; then nm -D --defined-only "$2" | awk '{print $NF}'; else readelf --dyn-syms -W "$2" | awk '$7 != "UND" && $5 == "GLOBAL" {print $8}'; fi | LC_ALL=C sort -u
    }
    needed() { readelf -d "$2" | awk '/\(NEEDED\)/{print $NF}' | tr -d '[]' | LC_ALL=C sort; }
    rpaths() { readelf -d "$2" | awk '/\((RPATH|RUNPATH)\)/{print $2, $NF}' | tr -d '[]'; }
    soname() { readelf -d "$2" | awk '/\(SONAME\)/{print $NF}' | tr -d '[]'; }
    unresolved() { env -u LD_LIBRARY_PATH ldd "$2" 2>&1 | grep -E 'not found' | awk '{print $1}' | LC_ALL=C sort; }
    compare exported-symbols exports
    compare needed-libraries needed
    compare rpath rpaths
    compare soname soname
    compare unresolved-libs unresolved
fi

echo
echo "== plug-ins loaded (RTLD_NOW, no LD_LIBRARY_PATH/DYLD_*)"
verify="$outdir/verify_plugin_loads"
if need "${CXX:-c++}"; then
    sed -e 's/RTLD_LAZY/RTLD_NOW/g' "$srcdir/openfx/HostSupport/src/ofxhBinary.cpp" > "$outdir/ofxhBinaryStrict.cpp"
    if "${CXX:-c++}" -std=c++17 -o "$verify" "$srcdir/.github/workflows/verify_plugin_loads.cpp" "$outdir/ofxhBinaryStrict.cpp" \
        "$srcdir/openfx/HostSupport/src/ofxhUtilities.cpp" -I "$srcdir/openfx/HostSupport/include" -I "$srcdir/openfx/include" \
        > "$outdir/verify-build.log" 2>&1 ||
        "${CXX:-c++}" -std=c++17 -o "$verify" "$srcdir/.github/workflows/verify_plugin_loads.cpp" "$outdir/ofxhBinaryStrict.cpp" \
        -I "$srcdir/openfx/HostSupport/include" -I "$srcdir/openfx/include" >> "$outdir/verify-build.log" 2>&1; then
        load() { env -u LD_LIBRARY_PATH -u DYLD_LIBRARY_PATH -u DYLD_FALLBACK_LIBRARY_PATH "$verify" "$2" 2>&1 | grep -v '^$'; }
        compare plug-ins load
        grep -h "Num_plugins" "$outdir/plug-ins.A.txt" "$outdir/plug-ins.B.txt" | sed -e 's/^/      /'
    else
        echo "  could not build the load checker (see $outdir/verify-build.log)"
        differ=1
    fi
fi

echo
if [ $differ = 0 ]; then
    echo "RESULT: equivalent"
else
    echo "RESULT: different (details above and in $outdir)"
fi
exit $differ
