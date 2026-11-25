#!/bin/bash
# Load and render every plug-in of an IO.ofx bundle with openfx's hostDemo (HostSupport/examples).
#
# The plug-ins are listed by the bundle itself (verify_plugin_loads). Each one must be covered by a test below, or by
# an explicit skip with its reason (SKIPS): a plug-in of the bundle that is neither fails the test, so that a new
# plug-in gets a test. hostDemo renders t = 0 and 1 (both views: it is built with the Nuke and Vegas extensions), with
# default parameters, on its default PAL SD input: at time t, the left view is red and the right view cyan, at grey
# level t (so black at t=0). The writers write files in the work directory, which the readers then read:
#   WritePNG -> ReadPNG, WriteEXR -> ReadEXR, WritePFM -> ReadPFM, WriteOIIO (TIFF, JPEG) -> ReadOIIO (TIFF, JPEG,
#   and the PNG and EXR files), WriteFFmpeg (MOV) -> ReadFFmpeg,
# and the images each reader renders are compared with those its writer rendered (compare.py, on hostDemo's --report
# files): the mean of each component must match, within a small tolerance for the lossless formats (PNG, TIFF, EXR,
# PFM), and a loose one for JPEG and MOV.
# ReadEXR and ReadOIIO also read EXR files that oiiotool writes with various data and display windows: ReadEXR's images
# must be the same as ReadOIIO's, with the expected bounds (see below).
# OCIOColorSpace swaps animated colorspaces: check_report.py checks its parameters' keys and values, and its images,
# in hostDemo's --report file (see below).
# A test fails on any load, describe, instance or render error (hostDemo's exit status), on a crash, on an error
# message of the plug-in (OfxMessageError, OfxMessageFatal), on a timeout, if an output frame of hostDemo is missing,
# if a writer's file is missing or empty, or if a reader's images differ from its writer's. A reader whose input file
# was not written (its writer failed, or is not in the bundle) fails too.
# Not tested: ReadOIIO's RAW support (see below).
#
# Usage: tests/hostdemo/run.sh <hostDemo> <IO.ofx.bundle> <verify_plugin_loads> [work dir]
# Environment:
#   OCIO                    default: tests/test.ocio, a minimal config with identity color spaces
#   HOSTDEMO_TIMEOUT        seconds per hostDemo run (default 120)
#   HOSTDEMO_TOTAL_TIMEOUT  seconds for all of them (default 600, less than ctest's TIMEOUT for io-hostdemo): the runs
#                           after that fail without running
#   IO_HOSTDEMO_STRICT      1 (as in CI): a bundle that is not installed, or a plug-in that is not in it, fails
#   HOSTDEMO_ARGS           more hostDemo arguments for every run, split on spaces, e.g. --ofx14 (io-hostdemo-ofx14:
#                           an OFX 1.4 host, which renders one view, into <prefix>.<t>.ppm)
#   PYTHON                  Python 3 (default: python3, or python), for compare.py
#   OIIOTOOL                OpenImageIO's oiiotool (default: oiiotool, on PATH), which writes the EXR window files:
#                           without it, those tests are skipped (they fail with IO_HOSTDEMO_STRICT=1)
# Runs on macOS, Linux and MSYS2. Exit status: 0 on success, 1 on failure, 77 if the bundle is not installed (1 with
# IO_HOSTDEMO_STRICT=1).
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
# kill_hung: the stacks of a hung hostDemo, then kill it (#244)
. "$SRC/tests/hostdemo/hang.sh"
WORK=$(abspath "${4:-${TMPDIR:-/tmp}/openfx-io-hostdemo}")

# MSYS2: hostDemo and the plug-ins are native programs, which need Windows paths
if command -v cygpath > /dev/null; then
    native() { cygpath -w "$1"; }
    HD=$(cygpath -u "$HD"); BUNDLE=$(cygpath -u "$BUNDLE"); VERIFY=$(cygpath -u "$VERIFY"); WORK=$(cygpath -u "$WORK")
else
    native() { printf '%s\n' "$1"; }
fi

STRICT=${IO_HOSTDEMO_STRICT:-0}
BINARY=$(ls "$BUNDLE"/Contents/*/IO.ofx 2> /dev/null | head -1)
if [ -z "$BINARY" ]; then
    echo "not installed: $BUNDLE (run cmake --install first)"
    [ "$STRICT" = 1 ] && { echo "FAIL: IO_HOSTDEMO_STRICT=1"; exit 1; }
    exit 77
fi
test -x "$HD" || { echo "error: hostDemo not found: $HD"; exit 1; }

TIMEOUT=${HOSTDEMO_TIMEOUT:-120}
TOTAL_TIMEOUT=${HOSTDEMO_TOTAL_TIMEOUT:-600}
PYTHON=${PYTHON:-$(command -v python3 || command -v python)}
if [ -z "$PYTHON" ] || ! "$PYTHON" -c 'import sys; sys.exit(sys.version_info[0] != 3)'; then
    echo "FAIL: Python 3 not found (set PYTHON), needed by compare.py"
    exit 1
fi
COMPARE=$(native "$SRC/tests/hostdemo/compare.py")
CHECK_REPORT=$(native "$SRC/tests/hostdemo/check_report.py")
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
echo "$BINARY: $NPLUGINS plug-ins${HOSTDEMO_ARGS:+ (hostDemo $HOSTDEMO_ARGS)}"
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

# hostDemo writes <prefix>.<t>.ppm, or <prefix>.<t>l.ppm and <prefix>.<t>r.ppm for the two views, for the times in
# $FRAMES (default: 0 1)
frames_ok() {
    local t
    for t in ${FRAMES:-0 1}; do
        [ -s "$1.$t.ppm" ] || { [ -s "$1.${t}l.ppm" ] && [ -s "$1.${t}r.ppm" ]; } || return 1
    done
}

# run <name> <plug-in id> <context> [needed file...] [-- hostDemo arguments...]
# Renders the plug-in; the test is called <name> (logs: out/<name>.log and out/<name>.json). The files before "--"
# must exist (non-empty) before the render, e.g. a reader's input; the file $WRITTEN, if set, must exist (non-empty)
# after it; if $COMPARE_WITH is set ("<writer test name> <tolerance> [--movie]"), compare.py must find the same
# images as the writer's; if $CHECK is set (check_report.py's checks, separated by spaces), they must pass on the
# --report file.
run() {
    local name=$1 id=$2 ctx=$3
    shift 3
    local needed=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do needed+=("$1"); shift; done
    [ "$1" = "--" ] && shift
    COVERED="$COVERED $id"
    if ! in_bundle "$id"; then
        if [ "$STRICT" = 1 ]; then
            echo "FAIL $name: $id is not in this bundle (its libraries were not found at build time; IO_HOSTDEMO_STRICT=1)"
            NFAIL=$((NFAIL + 1)); fail=1
            return 1
        fi
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
    # a hung run fails the test, after min(TIMEOUT, what is left of TOTAL_TIMEOUT) seconds
    local timeout=$((TOTAL_TIMEOUT - SECONDS))
    [ $timeout -gt "$TIMEOUT" ] && timeout=$TIMEOUT
    if [ $timeout -le 0 ]; then
        echo "FAIL $name: $id ($ctx): not run, the tests took more than ${TOTAL_TIMEOUT}s (HOSTDEMO_TOTAL_TIMEOUT)"
        NFAIL=$((NFAIL + 1)); fail=1
        return 1
    fi
    local status=0 pid watchdog
    rm -f "$OUT/$name".*
    # shellcheck disable=SC2086
    (cd "$OUT" && exec env OFX_PLUGIN_PATH="$PLUGPATH" "$HD" --quiet --no-std-paths --no-cache $HOSTDEMO_ARGS -p "$id" \
        -c "$ctx" -o "$(native "$OUT/$name")" --report "$(native "$OUT/$name.json")" "$@") > "$log" 2>&1 &
    pid=$!
    # the watchdog is a sleep, a child of this shell (no subshell whose own sleep could be orphaned): poll hostDemo,
    # and kill it if the watchdog has ended first
    sleep "$timeout" &
    watchdog=$!
    while kill -0 $pid 2> /dev/null; do
        if ! kill -0 $watchdog 2> /dev/null; then
            kill_hung $pid "$OUT/$name.stacks"
            echo "killed after ${timeout}s" >> "$log"
            break
        fi
        sleep 0.2
    done
    wait $pid || status=$?
    kill $watchdog 2> /dev/null
    wait $watchdog 2> /dev/null
    local why=""
    if grep -q "^killed after" "$log"; then
        why="timeout (${timeout}s)"
        # hostDemo writes the --report file after the render actions, before it unloads the plug-ins
        if [ -s "$OUT/$name.json" ]; then
            why="$why, after the render: out/$name.json was written, hostDemo hung later (at unload or exit)"
        else
            why="$why, in the actions: no out/$name.json"
        fi
        [ -s "$OUT/$name.stacks" ] && cat "$OUT/$name.stacks" >> "$log"
    elif [ $status -ne 0 ]; then
        why="hostDemo exit status $status"
        [ $status -gt 128 ] && why="$why (signal $((status - 128)))"
    elif grep -q "OfxMessageError\|OfxMessageFatal" "$log"; then
        why="error message"
    elif ! frames_ok "$OUT/$name"; then
        why="missing output frame"
    elif [ -n "$WRITTEN" ] && [ ! -s "$WRITTEN" ]; then
        why="did not write $WRITTEN"
    elif [ -n "$COMPARE_WITH" ]; then
        local writer tol mode
        read -r writer tol mode <<< "$COMPARE_WITH" # (mode: the rest, e.g. --crc)
        # shellcheck disable=SC2086
        if ! "$PYTHON" "$COMPARE" "$(native "$OUT/$name.json")" "$(native "$OUT/$writer.json")" "$tol" $mode \
             > "$OUT/$name.compare" 2>&1; then
            why="its images differ from those of $writer"
            cat "$OUT/$name.compare" >> "$log"
        fi
    elif [ -n "$CHECK" ]; then
        local checks
        # (split on spaces and newlines, without pathname expansion of the [[t,v]...] keys)
        read -r -d '' -a checks <<< "$CHECK"
        if ! "$PYTHON" "$CHECK_REPORT" "$(native "$OUT/$name.json")" "${checks[@]}" > "$OUT/$name.check" 2>&1; then
            why="its report fails check_report.py"
            cat "$OUT/$name.check" >> "$log"
        fi
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
    WRITTEN=$file COMPARE_WITH="" CHECK="" run "$name" "$id" general -- -s "filename=$(native "$file")" "$@"
}

# read <name> <plug-in id> <file> <writer test name> <tolerance> [hostDemo arguments...]
# Renders a reader in the general context, its filename param set to files/<file>, which must exist, and compares its
# images with those of the writer's test (compare.py).
read_() {
    local name=$1 id=$2 file=$FILES/$3 writer=$4 tol=$5
    shift 5
    WRITTEN="" COMPARE_WITH="$writer $tol" CHECK="" run "$name" "$id" general "$file" -- -s "filename=$(native "$file")" "$@"
}

P=fr.inria.openfx

# writers (general context: their Source is hostDemo's default input)
write WritePNG       $P.WritePNG    image.png
write WriteEXR       $P.WriteEXR    image.exr
# (and its 16-bit half path)
write WriteEXR-half  $P.WriteEXR    half.exr -s dataType=0
write WritePFM       $P.WritePFM    image.pfm
write WriteOIIO-tiff $P.WriteOIIO   image.tif
write WriteOIIO-jpeg $P.WriteOIIO   image.jpg
write WriteFFmpeg    $P.WriteFFmpeg movie.mov

# readers, on the files written above, compared with their writer (compare.py): the same bounds, and each component
# mean within LOSSLESS (8-bit quantization included) and the same CRC-32 of the images (so that a flipped or shifted
# image fails), or within LOSSY (JPEG, MOV)
LOSSLESS="0.001 --crc"
LOSSY=0.03
read_ ReadPNG        $P.ReadPNG     image.png WritePNG       "$LOSSLESS"
read_ ReadEXR        $P.ReadEXR     image.exr WriteEXR       "$LOSSLESS"
read_ ReadEXR-half   $P.ReadEXR     half.exr  WriteEXR-half  "$LOSSLESS"
read_ ReadPFM        $P.ReadPFM     image.pfm WritePFM       "$LOSSLESS"
read_ ReadOIIO-tiff  $P.ReadOIIO    image.tif WriteOIIO-tiff "$LOSSLESS"
read_ ReadOIIO-jpeg  $P.ReadOIIO    image.jpg WriteOIIO-jpeg $LOSSY
read_ ReadOIIO-png   $P.ReadOIIO    image.png WritePNG       "$LOSSLESS"
read_ ReadOIIO-exr   $P.ReadOIIO    image.exr WriteEXR       "$LOSSLESS"

# EXR data and display windows (#115). WriteEXR writes both windows at (0,0) and equal: oiiotool writes the files
# instead, a gradient (so that a shifted or flipped image fails) with these windows (OpenEXR, inclusive, y down):
#   name        display window    data window          RoD (OFX pixels, y up)
#   default     (0,0)-(99,59)     the same             0,0,100,60
#   offset      (5,7)-(104,66)    the same             0,0,100,60
#   crop        (0,0)-(719,575)   (10,20)-(109,69)     9,505,111,557
#   overscan    (0,0)-(99,59)     (-10,-10)-(109,69)   -11,-11,111,71
#   offsetcrop  (5,7)-(104,66)    (20,17)-(59,46)      14,19,56,51
# The display window is the format, moved to (0,0); the RoD is the data window in that frame, y flipped, grown by
# 1 pixel when the windows differ, as in ReadOIIO (Edge Pixels: Auto): that border, outside the data window, must be
# black, and not NaN (unwritten). ReadEXR must render the same images (bounds and CRC-32) as ReadOIIO, which must
# have the expected RoD, and neither may have NaN pixels. The render window is the whole RoD (--window), which
# hostDemo would otherwise clip to its project frame (the plug-ins don't support tiles).
EXRWIN="default:100x60::0,0,100,60
offset:100x60:--origin +5+7 --fullsize 100x60+5+7:0,0,100,60
crop:720x576:--crop 100x50+10+20:9,505,111,557
overscan:120x80:--origin -10-10 --fullsize 100x60+0+0:-11,-11,111,71
offsetcrop:100x60:--origin +5+7 --fullsize 100x60+5+7 --crop 40x30+20+17:14,19,56,51"
GRADIENT="fill:topleft=1,0,0,1:topright=0,1,0,1:bottomleft=0,0,1,0.5:bottomright=1,1,1,0.25"
OIIOTOOL=${OIIOTOOL:-$(command -v oiiotool)}
WINDOW="--window -1000,-1000,1000,1000"
while IFS=: read -r w size windows rod; do
    if [ -z "$OIIOTOOL" ]; then
        if [ "$STRICT" = 1 ]; then
            echo "FAIL ReadEXR-$w: oiiotool not found (set OIIOTOOL; IO_HOSTDEMO_STRICT=1)"
            NFAIL=$((NFAIL + 1)); fail=1
        else
            echo "SKIP: ReadEXR-$w, ReadOIIO-$w: oiiotool not found (set OIIOTOOL)"
        fi
        continue
    fi
    f=window-$w.exr
    rm -f "$FILES/$f"
    # (windows: oiiotool options, split into words)
    # shellcheck disable=SC2086
    if ! "$OIIOTOOL" --pattern "$GRADIENT" "$size" 4 $windows -d float -o "$(native "$FILES/$f")" < /dev/null > "$OUT/oiiotool-$w.log" 2>&1; then
        sed 's/^/     | /' "$OUT/oiiotool-$w.log"
    fi
    # ReadOIIO: the reference, compared with itself for its NaN pixels and its RoD
    read_ ReadOIIO-$w $P.ReadOIIO "$f" ReadOIIO-$w "$LOSSLESS --bounds=$rod" $WINDOW
    read_ ReadEXR-$w  $P.ReadEXR  "$f" ReadOIIO-$w "$LOSSLESS --bounds=$rod" $WINDOW
done <<< "$EXRWIN"
# the movie's frames 1 and 2 (its time domain) are the writer's renders at t=0 and 1, left view (compare.py --movie)
FRAMES="1 2" read_ ReadFFmpeg $P.ReadFFmpeg movie.mov WriteFFmpeg "$LOSSY --movie" --frames 1,2
# Not tested: ReadOIIO's RAW support (LibRaw). No writer makes a camera RAW file that LibRaw reads, so none can be
# generated at test time, and real camera RAW or DNG files are far larger than the few KB a test file may take.
echo "SKIP: ReadOIIO-raw: no RAW input (none can be generated at test time, and real RAW files are too large to commit)"

# filters, with default parameters, on hostDemo's default input (OCIO: the config in $OCIO)
WRITTEN=""
COMPARE_WITH=""
CHECK=""
run OCIOColorSpace    $P.OCIOColorSpace    filter
# OCIOColorSpace's Swap Colorspaces button, with an animated colorspace (#128), in a config where xN is N times the
# reference (so that x1 -> x2 halves the 0.25 input). The input colorspace menu (ocioInputSpaceIndex) has keys, x2 at
# t=0 and x4 at t=1; the output colorspace is x1 (the default, scene_linear). After a swap at t=0, the output menu has
# the input's keys and the input one is x1, without keys. hostDemo doesn't animate strings
# (kOfxParamHostPropSupportsStringAnimation=0): the colorspace string params, which the render uses, get the values at
# the time of the swap, so that the output one is x2 (the input's value at t=0, not x4, that of the last edit) and
# every render is x1 -> x2. Swapping again at t=1 moves the keys back to the input menu, and its string is x4, the
# value at t=1.
cp "$SRC/tests/swap.ocio" "$FILES/swap.ocio"
SWAP="-d float -i Source=solid:0.25,0.25,0.25,1 -s ocioInputSpaceIndex@0=1 -s ocioInputSpaceIndex@1=3"
SWAPOCIO=$(native "$FILES/swap.ocio")
# shellcheck disable=SC2086
OCIO=$SWAPOCIO CHECK="keys:ocioInputSpaceIndex=[] keys:ocioOutputSpaceIndex=[[0,1],[1,3]]
    value:ocioInputSpace=scene_linear value:ocioOutputSpace=x2 mean@0=0.125,0.125,0.125,1 mean@1=0.125,0.125,0.125,1" \
    run OCIOColorSpace-swap0  $P.OCIOColorSpace filter "$FILES/swap.ocio" -- $SWAP -s ocioSwapColorspaces@0
# shellcheck disable=SC2086
OCIO=$SWAPOCIO CHECK="keys:ocioInputSpaceIndex=[[0,1],[1,3]] keys:ocioOutputSpaceIndex=[]
    value:ocioInputSpace=x4 value:ocioOutputSpace=scene_linear mean@0=1,1,1,1 mean@1=1,1,1,1" \
    run OCIOColorSpace-swap01 $P.OCIOColorSpace filter "$FILES/swap.ocio" -- $SWAP -s ocioSwapColorspaces@0 \
    -s ocioSwapColorspaces@1
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
        echo "SKIP: $id: $reason"
        NSKIP=$((NSKIP + 1))
    else
        echo "FAIL $id: no test and no skip in $0"
        NFAIL=$((NFAIL + 1)); fail=1
    fi
done

echo "hostdemo${HOSTDEMO_ARGS:+ $HOSTDEMO_ARGS}: $NOK ok, $NFAIL failed, $NSKIP plug-ins skipped, $NABSENT tests of plug-ins not in the bundle (logs: $OUT)"
exit $fail
