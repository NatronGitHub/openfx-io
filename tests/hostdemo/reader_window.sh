#!/bin/bash
# Render the readers of a bundle with render windows smaller than the image, and partly or entirely out of its RoD
# (#232), with reader_window (reader_window.cpp, a host built on openfx's HostSupport): each window must give the
# same pixels as the same region of a full render, transparent black out of the RoD, and the reader must write only
# the window.
#
# Usage: tests/hostdemo/reader_window.sh <reader_window> <dir with the bundle> [work dir]
#
# The files are written in the work directory by oiiotool (PNG, TIFF, EXR), reader_window --write-pfm (PFM: not every
# oiiotool writes it) and ffmpeg (MOV); a file that can't be written is skipped, with its reason. Each reader is
# rendered with both filePremult values that select the two paths of GenericReader::render() at scale 1: decode()
# straight into the output image (an Opaque RGB file, a PreMultiplied RGBA file), and decode() into a temporary
# image that GenericReader then premultiplies (UnPreMultiplied); and at scale 1/2 (decode() into the temporary image,
# that GenericReader then downscales).
# Environment:
#   OCIO       default: tests/test.ocio
#   OIIOTOOL   default: oiiotool, on PATH
#   FFMPEG     default: ffmpeg, on PATH (without it, ReadFFmpeg is not tested)
#   READER_WINDOW_TIMEOUT  seconds per run (default 300)
#   IO_HOSTDEMO_STRICT     1 (as in CI, and as for run.sh): a missing bundle or oiiotool, a sample that can't be
#                          written, or no passed run fails the test instead of skipping
# Exit status: 0 on success, 1 on failure, 77 if the bundle or oiiotool is missing (1 with IO_HOSTDEMO_STRICT=1).
set -o pipefail

if [ $# -lt 2 ]; then
    echo "Usage: $0 <reader_window> <dir with the bundle> [work dir]"
    exit 1
fi
abspath() { case "$1" in /*|[A-Za-z]:[/\\]*) printf '%s\n' "$1" ;; *) printf '%s\n' "$PWD/$1" ;; esac; }
RW=$(abspath "$1")
PLUGINS=$(abspath "$2")
SRC=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(abspath "${3:-${TMPDIR:-/tmp}/openfx-io-reader-window}")
TIMEOUT=${READER_WINDOW_TIMEOUT:-300}

# MSYS2: reader_window and the plug-ins are native programs, which need Windows paths
if command -v cygpath > /dev/null; then
    native() { cygpath -w "$1"; }
else
    native() { printf '%s\n' "$1"; }
fi

STRICT=${IO_HOSTDEMO_STRICT:-0}
# skip the whole test (77), or fail it with IO_HOSTDEMO_STRICT=1
skipall() {
    if [ "$STRICT" = 1 ]; then
        echo "FAIL: $1 (IO_HOSTDEMO_STRICT=1)"
        exit 1
    fi
    echo "skip: $1"
    exit 77
}
if ! ls -d "$PLUGINS"/*.ofx.bundle > /dev/null 2>&1; then
    skipall "no .ofx.bundle in $PLUGINS"
fi
OIIOTOOL=${OIIOTOOL:-$(command -v oiiotool)}
if [ -z "$OIIOTOOL" ]; then
    skipall "oiiotool not found (set OIIOTOOL)"
fi
FFMPEG=${FFMPEG:-$(command -v ffmpeg)}
TIMEOUT_CMD=$(command -v timeout || command -v gtimeout)

rm -rf "$WORK"
mkdir -p "$WORK/files" "$WORK/logs"
export OFX_PLUGIN_PATH
OFX_PLUGIN_PATH=$(native "$PLUGINS")
export OCIO
OCIO=$(native "${OCIO:-$SRC/tests/test.ocio}")
# no plug-in cache, no user settings
export HOME="$WORK" XDG_CACHE_HOME="$WORK"

FILES="$WORK/files"
# files that differ along x and y, of odd sizes (the windows at scale 1/2 are rounded)
GRADIENT="fill:topleft=0.9,0.1,0.2,1:topright=0.1,0.8,0.3,0.5:bottomleft=0.2,0.3,0.9,0.25:bottomright=0.7,0.6,0.1,0.75"
# skip <file> <reason>: the file could not be written, its tests are skipped
skipped=""
skip() {
    echo "skip $1: $2"
    skipped="$skipped $1"
}
mk() { # <file> <channels> <oiiotool options...>
    local f=$1 n=$2
    shift 2
    if ! "$OIIOTOOL" --pattern "$GRADIENT" 61x47 "$n" "$@" -o "$(native "$FILES/$f")" < /dev/null > "$WORK/logs/oiiotool-$f.log" 2>&1 \
        || [ ! -s "$FILES/$f" ]; then
        sed 's/^/     | /' "$WORK/logs/oiiotool-$f.log" | head -5
        skip "$f" "oiiotool could not write it"
    fi
}
mk rgb.png 3 -d uint8
mk rgba16.png 4 -d uint16
mk rgb.tif 3 -d float
mk rgba.tif 4 -d float
# an EXR whose data window is smaller than its display window
mk datawindow.exr 3 -d half --crop 40x30+7+5 --fullsize 61x47+0+0
# a tiled RGBA EXR (ReadOIIO then supports tiles: it also gets images of the render window only)
mk tiled.exr 4 -d half --tile 16 16
if ! "$RW" --write-pfm "$(native "$FILES/rgb.pfm")" 61 47 > "$WORK/logs/write-pfm.log" 2>&1 || [ ! -s "$FILES/rgb.pfm" ]; then
    sed 's/^/     | /' "$WORK/logs/write-pfm.log" | head -5
    skip rgb.pfm "reader_window --write-pfm could not write it"
fi
if [ -z "$FFMPEG" ]; then
    skip testsrc.mov "ffmpeg not found (set FFMPEG)"
elif ! "$FFMPEG" -v error -nostdin -f lavfi -i testsrc=size=62x46:rate=24 -frames:v 3 -c:v prores_ks -pix_fmt yuv422p10le \
    -y "$(native "$FILES/testsrc.mov")" > "$WORK/logs/ffmpeg.log" 2>&1 || [ ! -s "$FILES/testsrc.mov" ]; then
    sed 's/^/     | /' "$WORK/logs/ffmpeg.log" | head -5
    skip testsrc.mov "ffmpeg could not write it"
fi

ok=0
failed=""
# check <identifier> <major> <file> [reader_window arguments...]
check() {
    local id=$1 major=$2 file=$3
    shift 3
    case " $skipped " in
    *" $file "*) return ;;
    esac
    local name="${id#fr.inria.openfx.}-$file${*:+ $*}"
    local log="$WORK/logs/$(printf '%s' "$name" | tr -c 'A-Za-z0-9.=-' '_').log"
    local cmd=("$RW" "$id" "$major" "filename=$(native "$FILES/$file")" "$@")
    if [ -n "$TIMEOUT_CMD" ]; then
        cmd=("$TIMEOUT_CMD" -k 10 "$TIMEOUT" "${cmd[@]}")
    fi
    if (cd "$WORK" && "${cmd[@]}") > "$log" 2>&1; then
        echo "ok   $name"
        ok=$((ok + 1))
    else
        echo "FAIL $name"
        grep -v '^ok ' "$log" | sed 's/^/     | /' | head -40
        failed="$failed $name"
    fi
}
# filePremult: 0 Opaque, 1 PreMultiplied, 2 UnPreMultiplied (the output is premultiplied)
for scale in 1 0.5; do
    check fr.inria.openfx.ReadPNG 1 rgb.png --scale $scale filePremult=0
    check fr.inria.openfx.ReadPNG 1 rgb.png --scale $scale filePremult=2
    check fr.inria.openfx.ReadPNG 1 rgba16.png --scale $scale filePremult=1
    check fr.inria.openfx.ReadPNG 1 rgba16.png --scale $scale filePremult=2
    check fr.inria.openfx.ReadPFM 1 rgb.pfm --scale $scale filePremult=0
    check fr.inria.openfx.ReadPFM 1 rgb.pfm --scale $scale filePremult=2
    check fr.inria.openfx.ReadOIIO 2 rgb.tif --scale $scale filePremult=0
    check fr.inria.openfx.ReadOIIO 2 rgb.tif --scale $scale filePremult=2
    check fr.inria.openfx.ReadOIIO 2 rgba.tif --scale $scale filePremult=1
    check fr.inria.openfx.ReadOIIO 2 rgba.tif --scale $scale filePremult=2
    check fr.inria.openfx.ReadOIIO 2 datawindow.exr --scale $scale filePremult=0
    check fr.inria.openfx.ReadOIIO 2 datawindow.exr --scale $scale filePremult=2
    check fr.inria.openfx.ReadOIIO 2 tiled.exr --scale $scale filePremult=1
    check fr.inria.openfx.ReadOIIO 2 tiled.exr --scale $scale filePremult=2
    check fr.inria.openfx.ReadFFmpeg 1 testsrc.mov --scale $scale --time 1 filePremult=0
    check fr.inria.openfx.ReadFFmpeg 1 testsrc.mov --scale $scale --time 1 filePremult=2
done

echo "$ok passed${skipped:+, skipped (file not written):$skipped}${failed:+, failed:$failed}"
if [ -n "$failed" ]; then
    exit 1
fi
# IO_HOSTDEMO_STRICT=1 (CI): every sample must be tested
if [ "$STRICT" = 1 ] && [ -n "$skipped" ]; then
    echo "FAIL: samples not written (IO_HOSTDEMO_STRICT=1):$skipped"
    exit 1
fi
if [ "$ok" -eq 0 ]; then
    echo "FAIL: no run passed"
    exit 1
fi
exit 0
