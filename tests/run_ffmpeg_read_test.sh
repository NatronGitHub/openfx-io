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
#   * VFR Matroska, MP4 and MOV (H.264 with B-frames, 24 fps) with timestamp gaps,
#     as in screen recordings or phone videos with dropped frames: a frame in a gap
#     must give the frame displayed at that time, i.e. the last frame at or before
#     it (--positions). The gaps are after a B-frame (24 frames, 2 frames apart
#     after frame 12), after the first GOP (frames 8-12 missing), and at the end
#     (a longer audio track makes the movie last longer than the last frame).
#   * VFR Matroska with a corrupted frame before a gap (8 bytes of its slice data
#     overwritten, so that it decodes with errors, concealed by the decoder): the
#     frames in the gap must be identical to the corrupted frame, as decoded at its own
#     frame (the frame displayed at that time), not an error or another frame. With a
#     long gap, and with a gap at the end, the number of seeks is bounded: the reader
#     must not decode from the start again for each frame in the gap.
#   * MOV with broken timestamps (the PTS of a P-frame and a B-frame are swapped, so
#     that one frame time has two frames and another one none): every frame must
#     decode, and give the same image sequentially and after a seek, except at these
#     two frame times (see check_swapped).
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

# check_vfr NAME DESCRIPTION FRAMES AFTER JUMP SPACING AUDIO_SECONDS EXPECTED_FRAMES FFMPEG_OUTPUT_ARGS...
# A 24 fps gray ramp of FRAMES frames with timestamp gaps: frame N (0-based) is at frame N
# up to AFTER, then at AFTER + JUMP + SPACING * (N - AFTER - 1). If AUDIO_SECONDS is not 0,
# a silent audio track of that duration is added, to make the movie longer. If
# EXPECTED_FRAMES is not 0, it is the number of frames the reader must report.
check_vfr() {
    local name="$1" desc="$2" n="$3" after="$4" jump="$5" spacing="$6" audio="$7" expected="$8"
    shift 8
    echo "== $desc =="
    if ! have_encoder libx264; then
        echo "SKIPPED: no libx264 encoder in the ffmpeg CLI"
        return
    fi
    local positions="" i pos
    for ((i = 0; i < n; i++)); do
        if [ "$i" -le "$after" ]; then
            pos=$i
        else
            pos=$((after + jump + spacing * (i - after - 1)))
        fi
        positions="${positions:+$positions,}$pos"
    done
    local audioin=()
    if [ "$audio" != 0 ]; then
        audioin=(-f lavfi -t "$audio" -i anullsrc=r=48000:cl=mono)
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i "$(ramp 24)" ${audioin[@]+"${audioin[@]}"} \
        -vf "trim=end_frame=$n,setpts=if(lte(N\,$after)\,N\,$after+$jump+$spacing*(N-$after-1))/(24*TB)" \
        -fps_mode passthrough $X264 "$@" "$tmp/$name"; then
        echo "FAIL: could not generate $name"
        rc=1
        return
    fi
    local framesarg=()
    if [ "$expected" != 0 ]; then
        framesarg=(--frames "$expected")
    fi
    # shellcheck disable=SC2086
    "$bin" "$tmp/$name" --tol 3 --ramp 16,4 --positions "$positions" ${framesarg[@]+"${framesarg[@]}"} ${FFMPEG_READ_TEST_ARGS:-} || rc=1
}

# The source for the clips with a corrupted frame: a textured ramp, so that the frames
# are large enough to be corrupted without losing the frame.
texramp() {
    echo "color=c=gray:s=128x128:r=24,format=gray,geq=lum='16+4*N+12*sin(X*0.7+N)*sin(Y*0.5)':cb=128:cr=128,format=yuv420p"
}

# check_corrupt NAME DESCRIPTION FRAMES JUMP SPACING AUDIO_SECONDS MAX_SEEKS
# A VFR textured ramp of FRAMES frames in Matroska with fixed B-frames, where frame N
# (0-based) is at frame N up to 12, then at 12 + JUMP + SPACING * (N - 13), and where the
# frame at frame 12, just before the first gap, is corrupted. If AUDIO_SECONDS is not 0,
# a silent audio track of that duration is added, to make the movie longer. If MAX_SEEKS
# is not 0, decoding every frame sequentially must not seek more than that.
check_corrupt() {
    local name="$1" desc="$2" n="$3" jump="$4" spacing="$5" audio="$6" maxseeks="$7"
    echo "== $desc =="
    if ! have_encoder libx264; then
        echo "SKIPPED: no libx264 encoder in the ffmpeg CLI"
        return
    fi
    local audioin=() audioout=()
    if [ "$audio" != 0 ]; then
        audioin=(-f lavfi -t "$audio" -i anullsrc=r=48000:cl=mono)
        audioout=(-c:a pcm_s16le)
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i "$(texramp)" ${audioin[@]+"${audioin[@]}"} \
        -vf "trim=end_frame=$n,setpts=if(lte(N\,12)\,N\,12+$jump+$spacing*(N-13))/(24*TB)" -fps_mode passthrough \
        -c:v libx264 -g 8 -bf 2 -x264-params b-adapt=0 -pix_fmt yuv420p ${audioout[@]+"${audioout[@]}"} "$tmp/clean_$name"; then
        echo "FAIL: could not generate clean_$name"
        rc=1
        return
    fi
    # Overwrite 8 bytes at 2/3 of the packet of frame 12 (Matroska timestamps are in
    # milliseconds: it is at 500 ms), in the slice data: the frame still decodes, with errors
    # (the bitstream filter noise may instead make the packet undecodable).
    if ! command -v ffprobe >/dev/null 2>&1; then
        echo "SKIPPED: no ffprobe CLI"
        return
    fi
    local pos size
    read -r pos size < <(ffprobe -v error -select_streams v:0 -show_entries packet=pts,pos,size -of compact "$tmp/clean_$name" |
        awk -F'|' '{ delete v; for (i = 2; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] } if (v["pts"] == 500) print v["pos"], v["size"] }')
    if [ -z "${pos:-}" ] || [ -z "${size:-}" ]; then
        echo "FAIL: no packet at 500 ms in clean_$name"
        rc=1
        return
    fi
    cp "$tmp/clean_$name" "$tmp/$name"
    printf '\377\377\377\377\377\377\377\377' | dd of="$tmp/$name" bs=1 seek=$((pos + size * 2 / 3)) conv=notrunc 2>/dev/null
    local positions="" i
    for ((i = 0; i < n; i++)); do
        positions="${positions:+$positions,}$((i <= 12 ? i : 12 + jump + spacing * (i - 13)))"
    done
    local seeksarg=()
    if [ "$maxseeks" != 0 ]; then
        seeksarg=(--max-seeks "$maxseeks")
    fi
    # shellcheck disable=SC2086
    "$bin" "$tmp/$name" --tol 3 --positions "$positions" ${seeksarg[@]+"${seeksarg[@]}"} ${FFMPEG_READ_TEST_ARGS:-} 2>/dev/null || rc=1
}

# check_swapped NAME DESCRIPTION
# A 24-frame CFR ramp in MOV (time scale 24) where the PTS of frames 10 and 11 are swapped:
# the decoder then outputs the timestamps 9, 11, 11, 12 (best_effort_timestamp).
# At frames 10 and 11 (0-based), the image after a seek may differ from the sequential one:
# libavcodec chooses best_effort_timestamp between the PTS and the DTS from the number of
# faulty timestamps seen so far, and in FFmpeg 8, avcodec_flush_buffers() does not reset
# these counts. After decoding the swapped PTS once, a seek back to frame 8 then gives the
# timestamps 9, 10, 11, 12 (from the DTS), while sequential decoding gives 9, 11, 11, 12
# (FFmpeg 9 gives 9, 11, 11, 12 in both cases). Only reopening the decoder at each seek
# would make this independent of what was decoded before, which is too costly. Both frames
# must still decode.
check_swapped() {
    local name="$1" desc="$2"
    echo "== $desc =="
    if ! have_encoder libx264; then
        echo "SKIPPED: no libx264 encoder in the ffmpeg CLI"
        return
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i "$(ramp 24)" -frames:v 24 $X264 -video_track_timescale 24 "$tmp/clean_$name"; then
        echo "FAIL: could not generate clean_$name"
        rc=1
        return
    fi
    if ! ffmpeg -y -loglevel error -i "$tmp/clean_$name" -c copy -video_track_timescale 24 \
        -bsf:v "setts=pts=if(eq(PTS\,10)\,11\,if(eq(PTS\,11)\,10\,PTS))" "$tmp/$name" 2>/dev/null; then
        echo "SKIPPED: no setts bitstream filter"
        return
    fi
    # shellcheck disable=SC2086
    "$bin" "$tmp/$name" --tol 3 --frames 24 --no-seek-check 11,12 ${FFMPEG_READ_TEST_ARGS:-} 2>/dev/null || rc=1
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
for ext in mkv mp4 mov; do
    EXT="$(echo "$ext" | tr '[:lower:]' '[:upper:]')"
    check_vfr "ramp_gaps.$ext" "VFR $EXT (H.264, 24 fps, gaps after a B-frame)" 24 12 2 2 0 0
    check_vfr "ramp_gop_gap.$ext" "VFR $EXT (H.264, 24 fps, gap after the first GOP)" 24 7 6 1 0 0
done
check_vfr ramp_end_gap.mkv "VFR MKV (H.264, 24 fps, gaps and a gap at the end)" 20 12 2 2 1.3333 32 -c:a pcm_s16le
check_vfr ramp_end_gap.mov "VFR MOV (H.264, 24 fps, gaps and a gap at the end)" 20 12 2 2 1.3333 32 -c:a pcm_s16le
check_corrupt ramp_corrupt_gap.mkv "VFR MKV (H.264, 24 fps, corrupted B-frame before a gap)" 24 2 2 0 0
# Sequentially, a seek for frame 1, and one to decode from the start when the corrupted frame
# is first held: the following frames in the gap must not seek.
check_corrupt ramp_corrupt_long_gap.mkv "VFR MKV (H.264, 24 fps, corrupted B-frame before a 20-frame gap)" 24 21 1 0 2
check_corrupt ramp_corrupt_end_gap.mkv "VFR MKV (H.264, 24 fps, corrupted last frame before a 35-frame gap at the end)" 13 0 0 2 2
check_swapped ramp_swapped.mov "MOV (H.264, 24 fps, broken timestamps: two PTS swapped)"

if [ "$rc" -eq 0 ]; then
    echo "ALL READ-PATH TESTS PASSED"
else
    echo "READ-PATH TESTS FAILED"
fi
exit "$rc"
