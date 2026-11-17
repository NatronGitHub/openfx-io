#!/usr/bin/env bash
#
# Generates known synthetic clips and runs the FFmpeg reader-core harness against
# them. Each clip is a 40-frame "gray ramp": frame N (0-based) has luma 16 + 4*N, so
# the frame number is in the pixels. The harness decodes sequentially, then
# re-decodes in a seek-forcing order, and checks that every decoded frame, sequential
# or seeked, holds the content of the requested frame (--ramp 16,4), that the reader
# reports 40 frames, and that seeked frames match the sequential ones. This validates
# frame-accurate decoding and seeking, and catches wrong frames as well as missing ones.
#
# Containers and codecs:
#   * MP4  (H.264 with B-frames, start_time == 0, edit list for the B-frame delay)
#   * MP4  (H.264, non-zero start time: -output_ts_offset)
#   * MOV  (H.264, 24 fps)
#   * MPEG-TS (H.264, start_time ~1.48s) -- non-zero stream start PTS, which is the
#     case that stresses the frame<->PTS seek arithmetic (Stream::frameToPts).
#   * Matroska (H.264) at 23.976, 24, 25, 30 and 60 fps, and ProRes at 24 fps: Matroska
#     stores timestamps in milliseconds, which are not a whole number of frames, and
#     Stream::ptsToFrame() must round them to the nearest frame.
#   * WebM (VP9 and VP8, 24 fps): also millisecond timestamps.
#   * VFR Matroska (H.264, 30 fps with timestamps jittered by up to +-6 ms, as in
#     screen recordings).
# Clips whose encoder is not available in the ffmpeg CLI are skipped.
#
# Environment:
#   FFMPEG_READ_TEST_KEEP=dir  generate the clips in dir and keep them
#   FFMPEG_READ_TEST_ARGS=...  extra harness arguments (e.g. --verbose)
#
# Exit status: 0 = all passed, non-zero = a failure was detected.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
bin="$here/ffmpeg_read_test"
if [ ! -x "$bin" ]; then
    echo "error: $bin not built. Run 'make' first." >&2
    exit 2
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "error: ffmpeg CLI not found (needed to generate test clips)." >&2
    exit 2
fi

if [ -n "${FFMPEG_READ_TEST_KEEP:-}" ]; then
    tmp="$FFMPEG_READ_TEST_KEEP"
    mkdir -p "$tmp"
else
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
fi

NFRAMES=40
ramp() {
    # ramp RATE: the lavfi source of a gray ramp at RATE fps
    echo "color=c=gray:s=128x128:r=$1,format=gray,geq=lum='16+4*N':cb=128:cr=128,format=yuv420p"
}
have_encoder() {
    ffmpeg -hide_banner -h "encoder=$1" 2>/dev/null | grep -q "^Encoder $1 "
}
X264="-c:v libx264 -g 8 -bf 2 -pix_fmt yuv420p"

rc=0
# check NAME DESCRIPTION ENCODER RATE FFMPEG_OUTPUT_ARGS...
check() {
    local name="$1" desc="$2" encoder="$3" rate="$4"
    shift 4
    echo "== $desc =="
    if ! have_encoder "$encoder"; then
        echo "SKIPPED: no $encoder encoder in the ffmpeg CLI"
        return
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i "$(ramp "$rate")" -frames:v "$NFRAMES" "$@" "$tmp/$name"; then
        echo "FAIL: could not generate $name"
        rc=1
        return
    fi
    # shellcheck disable=SC2086
    "$bin" "$tmp/$name" --monotonic --tol 3 --ramp 16,4 --frames "$NFRAMES" ${FFMPEG_READ_TEST_ARGS:-} || rc=1
}

echo "Generating test clips in $tmp and checking them..."
check ramp.mp4 "MP4 (H.264, start_time 0)" libx264 25 $X264
check ramp_offset.mp4 "MP4 (H.264, 24 fps, non-zero start time)" libx264 24 $X264 -output_ts_offset 10
check ramp.mov "MOV (H.264, 24 fps)" libx264 24 $X264
check ramp.ts "MPEG-TS (H.264, non-zero start_time)" libx264 25 $X264 -f mpegts
check ramp_23.976.mkv "Matroska (H.264, 23.976 fps)" libx264 24000/1001 $X264
check ramp_24.mkv "Matroska (H.264, 24 fps)" libx264 24 $X264
check ramp_25.mkv "Matroska (H.264, 25 fps)" libx264 25 $X264
check ramp_30.mkv "Matroska (H.264, 30 fps)" libx264 30 $X264
check ramp_60.mkv "Matroska (H.264, 60 fps)" libx264 60 $X264
check ramp_prores_24.mkv "Matroska (ProRes, 24 fps)" prores_ks 24 -c:v prores_ks
check ramp_vp9_24.webm "WebM (VP9, 24 fps)" libvpx-vp9 24 -c:v libvpx-vp9 -g 8 -crf 10 -b:v 0
check ramp_vp8_24.webm "WebM (VP8, 24 fps)" libvpx 24 -c:v libvpx -g 8 -crf 4 -b:v 10M
check ramp_vfr_30.mkv "VFR Matroska (H.264, 30 fps, jittered timestamps)" libx264 30 \
    -vf "settb=1/1000,setpts=(N/30+0.006*sin(N*1.7))/TB" -fps_mode passthrough -enc_time_base 1/1000 $X264

if [ "$rc" -eq 0 ]; then
    echo "ALL READ-PATH TESTS PASSED"
else
    echo "READ-PATH TESTS FAILED"
fi
exit "$rc"
