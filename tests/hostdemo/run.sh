#!/bin/bash
# Load and render every plug-in of an IO.ofx bundle with openfx's hostDemo (HostSupport/examples).
#
# The plug-ins are listed by the bundle itself (verify_plugin_loads). Each one must be covered by a test below, or by
# an explicit skip with its reason (SKIPS): a plug-in of the bundle that is neither fails the test, so that a new
# plug-in gets a test. hostDemo renders t = 0 and 1 (both views: it is built with the Nuke and Vegas extensions), with
# default parameters, on its default PAL SD input (black at t=0, white at t=1). The writers write files in the work
# directory, which the readers then read:
#   WritePNG -> ReadPNG, WriteEXR -> ReadEXR, WritePFM -> ReadPFM, WriteOIIO (TIFF, JPEG) -> ReadOIIO (TIFF, JPEG,
#   and the PNG and EXR files), WriteFFmpeg (MOV) -> ReadFFmpeg.
# A test fails on any load, describe, instance or render error (hostDemo's exit status), on a crash, on an error
# message of the plug-in (OfxMessageError), on a timeout, if an output frame of hostDemo is missing, or if a writer's file is
# missing or empty. A reader whose input file was not written (its writer failed, or is not in the bundle) fails too.
# Not tested: ReadOIIO's RAW support (see below).
#
# Usage: tests/hostdemo/run.sh <hostDemo> <IO.ofx.bundle> <verify_plugin_loads> [work dir]
# Environment: OCIO (default: tests/test.ocio, a minimal config with identity color spaces), HOSTDEMO_TIMEOUT
# (seconds per render, default 120).
# Runs on macOS, Linux and MSYS2. Exit status: 0 on success, 1 on failure, 77 if the bundle is not installed.
set -o pipefail

if [ $# -lt 3 ]; then
    echo "Usage: $0 <hostDemo> <IO.ofx.bundle> <verify_plugin_loads> [work dir]"
    exit 1
fi
# absolute paths (hostDemo runs in the output directory)
abspath() { case "$1" in /*|[A-Za-z]:[/\\]*) printf '%s\n' "$1" ;; *) printf '%s\n' "$PWD/$1" ;; esac; }
HD=$(abspath "$1")
BUNDLE=$(abspath "$2")
VERIFY=$(abspath "$3")
SRC=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(abspath "${4:-${TMPDIR:-/tmp}/openfx-io-hostdemo}")

# MSYS2: hostDemo and the plug-ins are native programs, which need Windows paths
if command -v cygpath > /dev/null; then
    native() { cygpath -w "$1"; }
    HD=$(cygpath -u "$HD"); BUNDLE=$(cygpath -u "$BUNDLE"); VERIFY=$(cygpath -u "$VERIFY"); WORK=$(cygpath -u "$WORK")
else
    native() { printf '%s\n' "$1"; }
fi

BINARY=$(ls "$BUNDLE"/Contents/*/IO.ofx 2> /dev/null | head -1)
if [ -z "$BINARY" ]; then
    echo "not installed: $BUNDLE (run cmake --install first)"
    exit 77
fi
test -x "$HD" || { echo "error: hostDemo not found: $HD"; exit 1; }

TIMEOUT=${HOSTDEMO_TIMEOUT:-120}
export OCIO=${OCIO:-$(native "$SRC/tests/test.ocio")}
OUT=$WORK/out
FILES=$WORK/files
rm -rf "$WORK/plugins" "$OUT" "$FILES"
mkdir -p "$WORK/plugins" "$OUT" "$FILES"
# only this bundle in OFX_PLUGIN_PATH (a copy: symbolic links are not followed on every OS)
cp -R "$BUNDLE" "$WORK/plugins/"
PLUGPATH=$(native "$WORK/plugins")

# --- the plug-ins of the bundle ---------------------------------------------------------------------------------
if ! "$VERIFY" "$BINARY" > "$WORK/plugins.txt" 2>&1; then
    cat "$WORK/plugins.txt"
    echo "FAIL: the bundle doesn't load"
    exit 1
fi
# "plugin[0] : fr.inria.openfx.ReadEXR v1.0"
PLUGINS=$(sed -n 's/^plugin\[[0-9]*\] : \([^ ]*\) v.*/\1/p' "$WORK/plugins.txt" | tr -d '\r' | tr '\n' ' ')
NPLUGINS=$(echo $PLUGINS | wc -w | tr -d ' ')
echo "$BINARY: $NPLUGINS plug-ins"
[ "$NPLUGINS" -gt 0 ] || { cat "$WORK/plugins.txt"; echo "FAIL: no plug-in"; exit 1; }

in_bundle() {
    case " $PLUGINS " in *" $1 "*) return 0 ;; esac
    return 1
}

# --- explicit skips: "plug-in identifier|reason" lines ------------------------------------------------------------
# (none at the moment)
SKIPS=""

# --- tests ------------------------------------------------------------------------------------------------------
fail=0
COVERED=""
NOK=0
NFAIL=0
NSKIP=0
NABSENT=0

# hostDemo writes <prefix>.<t>.ppm, or <prefix>.<t>l.ppm and <prefix>.<t>r.ppm for the two views
frames_ok() {
    local t
    for t in 0 1; do
        [ -s "$1.$t.ppm" ] || { [ -s "$1.${t}l.ppm" ] && [ -s "$1.${t}r.ppm" ]; } || return 1
    done
}

# run <name> <plug-in id> <context> [needed file...] [-- hostDemo arguments...]
# Renders the plug-in; the test is called <name> (logs: out/<name>.log and out/<name>.json). The files before "--"
# must exist (non-empty) before the render, e.g. a reader's input; the file $WRITTEN, if set, must exist (non-empty)
# after it.
run() {
    local name=$1 id=$2 ctx=$3
    shift 3
    local needed=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do needed+=("$1"); shift; done
    [ "$1" = "--" ] && shift
    COVERED="$COVERED $id"
    if ! in_bundle "$id"; then
        echo "--   $name: $id is not in this bundle (its libraries were not found at build time)"
        NABSENT=$((NABSENT + 1))
        return 0
    fi
    local log="$OUT/$name.log" f
    for f in "${needed[@]}"; do
        if [ ! -s "$f" ]; then
            echo "FAIL $name: $id ($ctx): its input $f was not written"
            NFAIL=$((NFAIL + 1)); fail=1
            return 1
        fi
    done
    local status=0 pid watchdog
    (cd "$OUT" && exec env OFX_PLUGIN_PATH="$PLUGPATH" "$HD" --quiet --no-std-paths --no-cache -p "$id" -c "$ctx" \
        -o "$(native "$OUT/$name")" --report "$(native "$OUT/$name.json")" "$@") > "$log" 2>&1 &
    pid=$!
    # a hung render fails the test (TIMEOUT seconds)
    (sleep "$TIMEOUT" && kill -9 $pid && echo "killed after ${TIMEOUT}s" >> "$log") > /dev/null 2>&1 &
    watchdog=$!
    wait $pid || status=$?
    kill $watchdog > /dev/null 2>&1
    wait $watchdog 2> /dev/null
    local why=""
    if grep -q "^killed after" "$log"; then
        why="timeout (${TIMEOUT}s)"
    elif [ $status -ne 0 ]; then
        why="hostDemo exit status $status"
        [ $status -gt 128 ] && why="$why (signal $((status - 128)))"
    elif grep -q "OfxMessageError" "$log"; then
        why="error message"
    elif ! frames_ok "$OUT/$name"; then
        why="missing output frame"
    elif [ -n "$WRITTEN" ] && [ ! -s "$WRITTEN" ]; then
        why="did not write $WRITTEN"
    fi
    if [ -n "$why" ]; then
        echo "FAIL $name: $id ($ctx): $why"
        sed 's/^/     | /' "$log"
        echo
        NFAIL=$((NFAIL + 1)); fail=1
        return 1
    fi
    echo "ok   $name: $id ($ctx)"
    NOK=$((NOK + 1))
    return 0
}

# write <name> <plug-in id> <file> [hostDemo arguments...]
# Renders a writer in the general context, its filename param set to files/<file>, which must then exist.
# (No sequence pattern such as name.####.png: the host expands it, as Natron does, and hostDemo doesn't. Both frames
# are written to the same file.)
write() {
    local name=$1 id=$2 file=$FILES/$3
    shift 3
    rm -f "$file"
    WRITTEN=$file run "$name" "$id" general -- -s "filename=$(native "$file")" "$@"
}

# read <name> <plug-in id> <file> [hostDemo arguments...]
# Renders a reader in the general context, its filename param set to files/<file>, which must exist.
read_() {
    local name=$1 id=$2 file=$FILES/$3
    shift 3
    WRITTEN="" run "$name" "$id" general "$file" -- -s "filename=$(native "$file")" "$@"
}

P=fr.inria.openfx

# writers (general context: their Source is hostDemo's default input)
write WritePNG       $P.WritePNG    image.png
write WriteEXR       $P.WriteEXR    image.exr
write WritePFM       $P.WritePFM    image.pfm
write WriteOIIO-tiff $P.WriteOIIO   image.tif
write WriteOIIO-jpeg $P.WriteOIIO   image.jpg
write WriteFFmpeg    $P.WriteFFmpeg movie.mov

# readers, on the files written above
read_ ReadPNG        $P.ReadPNG     image.png
read_ ReadEXR        $P.ReadEXR     image.exr
read_ ReadPFM        $P.ReadPFM     image.pfm
read_ ReadOIIO-tiff  $P.ReadOIIO    image.tif
read_ ReadOIIO-jpeg  $P.ReadOIIO    image.jpg
read_ ReadOIIO-png   $P.ReadOIIO    image.png
read_ ReadOIIO-exr   $P.ReadOIIO    image.exr
read_ ReadFFmpeg     $P.ReadFFmpeg  movie.mov
# Not tested: ReadOIIO's RAW support (LibRaw). No writer makes a camera RAW file that LibRaw reads, so none can be
# generated at test time, and real camera RAW or DNG files are far larger than the few KB a test file may take.
echo "skip ReadOIIO-raw: no RAW input (none can be generated at test time, and real RAW files are too large to commit)"

# filters, with default parameters, on hostDemo's default input (OCIO: the config in $OCIO)
WRITTEN=""
run OCIOColorSpace    $P.OCIOColorSpace    filter
run OCIODisplay       $P.OCIODisplay       filter
run OCIOCDLTransform  $P.OCIOCDLTransform  filter
# OCIOFileTransform has no default file (it fails with "FileTransform: empty file path"): an identity 1D LUT
printf 'LUT_1D_SIZE 2\n0 0 0\n1 1 1\n' > "$FILES/identity.cube"
run OCIOFileTransform $P.OCIOFileTransform filter "$FILES/identity.cube" -- -s "file=$(native "$FILES/identity.cube")"
run OCIOLogConvert    $P.OCIOLogConvert    filter
run OCIOLookTransform $P.OCIOLookTransform filter
# (OIIOText needs one of OpenImageIO's default fonts, DroidSans, cour, Courier New or FreeMono, or else it reports
# "Could not set default font face": Ubuntu's fonts-freefont-ttf has FreeMono)
run OIIOText          $P.OIIOText          filter
run OIIOResize        $P.OIIOResize        filter
# SeExpr and SeExprSimple, in hosts other than Natron, and RunScript don't render until their script is validated
# ("Validate the script before rendering/running."): validate the default one. RunScript's default script does
# nothing (#!/bin/sh, or "@echo off" on MS Windows).
run SeExpr            $P.SeExpr            filter -- -s validate=1
run SeExprSimple      $P.SeExprSimple      filter -- -s validate=1
run SeGrain           net.sf.openfx.SeGrain filter
run SeNoise           net.sf.openfx.SeNoise filter
run RunScript         $P.RunScript         filter -- -s validate=1

# --- every plug-in of the bundle is tested or skipped ------------------------------------------------------------
for id in $PLUGINS; do
    case " $COVERED " in *" $id "*) continue ;; esac
    reason=$(printf '%s\n' "$SKIPS" | sed -n "s/^$id|//p")
    if [ -n "$reason" ]; then
        echo "skip $id: $reason"
        NSKIP=$((NSKIP + 1))
    else
        echo "FAIL $id: no test and no skip in $0"
        NFAIL=$((NFAIL + 1)); fail=1
    fi
done

echo "hostdemo: $NOK ok, $NFAIL failed, $NSKIP plug-ins skipped, $NABSENT tests of plug-ins not in the bundle (logs: $OUT)"
exit $fail
