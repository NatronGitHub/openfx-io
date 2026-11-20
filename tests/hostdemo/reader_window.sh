#!/bin/bash
# Render the readers of a bundle with render windows smaller than the image, and partly or entirely out of its RoD
# (#232), with reader_window (reader_window.cpp, a host built on openfx's HostSupport): each window must give the
# same pixels as the same region of a full render, transparent black out of the RoD, and the reader must write only
# the window.
#
# Usage: tests/hostdemo/reader_window.sh <reader_window> <dir with the bundle> [work dir]
#
# The files are written in the work directory by oiiotool (PNG, TIFF, EXR, PFM) and ffmpeg (MOV). Each reader is
# rendered with both filePremult values that select the two paths of GenericReader::render() at scale 1: decode()
# straight into the output image (an Opaque RGB file, a PreMultiplied RGBA file), and decode() into a temporary
# image that GenericReader then premultiplies (UnPreMultiplied); and at scale 1/2 (decode() into the temporary image,
# that GenericReader then downscales).
# Environment:
#   OCIO       default: tests/test.ocio
#   OIIOTOOL   default: oiiotool, on PATH
#   FFMPEG     default: ffmpeg, on PATH (without it, ReadFFmpeg is not tested)
#   READER_WINDOW_TIMEOUT  seconds per run (default 300)
# Exit status: 0 on success, 1 on failure, 77 if the bundle or oiiotool is missing.
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

if ! ls -d "$PLUGINS"/*.ofx.bundle > /dev/null 2>&1; then
    echo "skip: no .ofx.bundle in $PLUGINS"
    exit 77
fi
OIIOTOOL=${OIIOTOOL:-$(command -v oiiotool)}
if [ -z "$OIIOTOOL" ]; then
    echo "skip: oiiotool not found (set OIIOTOOL)"
    exit 77
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
mk() { # <file> <channels> <oiiotool options...>
    local f=$1 n=$2
    shift 2
    if ! "$OIIOTOOL" --pattern "$GRADIENT" 61x47 "$n" "$@" -o "$(native "$FILES/$f")" < /dev/null > "$WORK/logs/oiiotool-$f.log" 2>&1; then
        sed 's/^/     | /' "$WORK/logs/oiiotool-$f.log"
        return 1
    fi
}
status=0
mk rgb.png 3 -d uint8 || status=1
mk rgba16.png 4 -d uint16 || status=1
mk rgb.pfm 3 -d float || status=1
mk rgb.tif 3 -d float || status=1
mk rgba.tif 4 -d float || status=1
# an EXR whose data window is smaller than its display window
mk datawindow.exr 3 -d half --crop 40x30+7+5 --fullsize 61x47+0+0 || status=1
# a tiled RGBA EXR (ReadOIIO then supports tiles: it also gets images of the render window only)
mk tiled.exr 4 -d half --tile 16 16 || status=1
MOV=""
if [ -n "$FFMPEG" ]; then
    if "$FFMPEG" -v error -nostdin -f lavfi -i testsrc=size=62x46:rate=24 -frames:v 3 -c:v prores_ks -pix_fmt yuv422p10le \
        -y "$(native "$FILES/testsrc.mov")" > "$WORK/logs/ffmpeg.log" 2>&1; then
        MOV=testsrc.mov
    else
        sed 's/^/     | /' "$WORK/logs/ffmpeg.log"
        status=1
    fi
else
    echo "skip ReadFFmpeg: ffmpeg not found (set FFMPEG)"
fi

ok=0
failed=""
# check <identifier> <major> <file> [reader_window arguments...]
check() {
    local id=$1 major=$2 file=$3
    shift 3
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
    if [ -n "$MOV" ]; then
        check fr.inria.openfx.ReadFFmpeg 1 "$MOV" --scale $scale --time 1 filePremult=0
        check fr.inria.openfx.ReadFFmpeg 1 "$MOV" --scale $scale --time 1 filePremult=2
    fi
done

echo "$ok passed${failed:+, failed:$failed}"
if [ -n "$failed" ] || [ $status -ne 0 ]; then
    exit 1
fi
exit 0
