#!/usr/bin/env bash
#
# Generates known synthetic clips and runs the FFmpeg reader-core harness against
# them. Each clip is a 40-frame "gray ramp": frame N has luma 16 + 2*N, so
# per-frame signatures are strictly increasing and distinct. The harness decodes
# sequentially (ground truth), then re-decodes in a seek-forcing order and checks
# every seeked frame matches — validating frame-accurate decoding and seeking.
#
# Two containers are exercised:
#   * MP4  (H.264, start_time == 0)
#   * MPEG-TS (H.264, start_time ~1.48s) -- non-zero stream start PTS, which is the
#     case that stresses the frame<->PTS seek arithmetic (Stream::frameToPts).
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

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

FILTER="color=c=gray:s=128x128:r=25,format=gray,geq=lum='16+2*N':cb=128:cr=128,format=yuv420p"

echo "Generating test clips..."
ffmpeg -y -loglevel error -f lavfi -i "$FILTER" -frames:v 40 \
    -c:v libx264 -g 8 -bf 2 -pix_fmt yuv420p "$tmp/ramp.mp4"
ffmpeg -y -loglevel error -f lavfi -i "$FILTER" -frames:v 40 \
    -c:v libx264 -g 8 -bf 2 -pix_fmt yuv420p -f mpegts "$tmp/ramp.ts"

rc=0
echo "== MP4 (H.264, start_time 0) =="
"$bin" "$tmp/ramp.mp4" --monotonic --tol 3 || rc=1
echo "== MPEG-TS (H.264, non-zero start_time) =="
"$bin" "$tmp/ramp.ts" --monotonic --tol 3 || rc=1

if [ "$rc" -eq 0 ]; then
    echo "ALL READ-PATH TESTS PASSED"
else
    echo "READ-PATH TESTS FAILED"
fi
exit "$rc"
