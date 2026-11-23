#!/usr/bin/env bash
#
# Generates known synthetic clips and runs the FFmpeg reader-core harness against
# them. Each clip is a 40-frame "gray ramp": frame N (0-based) has luma 16 + 4*N, so
# the frame number is in the pixels. The harness decodes sequentially, then
# re-decodes out of order (backward and long jumps, which seek, and short forward jumps,
# which may decode forward without a seek, #311), and checks that every decoded frame,
# sequential or out of order, holds the content of the requested frame (--ramp 16,4), that
# the reader reports 40 frames, and that out-of-order frames match the sequential ones. This validates
# frame-accurate decoding and seeking, and catches wrong frames as well as missing ones.
#
# Containers and codecs:
#   * MP4  (H.264 with B-frames, start_time == 0, edit list for the B-frame delay)
#   * MP4  (H.264, non-zero start time: -output_ts_offset)
#   * MOV  (H.264, 24 fps)
#   * MPEG-TS (H.264, start_time ~1.48s) -- non-zero stream start PTS, which is the
#     case that stresses the frame<->PTS seek arithmetic (Stream::frameToPts).
#   * M2TS (AVCHD-like: interlaced H.264 at 29.97 fps, with an AC3 audio track that starts
#     before the video): the frames are counted from the start of the video stream.
#   * Matroska (H.264) at 23.976, 24, 25, 30 and 60 fps, and ProRes at 24 fps: Matroska
#     stores timestamps in milliseconds, which are not a whole number of frames, and
#     Stream::ptsToFrame() must round them to the nearest frame.
#   * Matroska and FLV (H.264 with B-frames) with a non-zero start time: their duration is
#     not counted from the start of the movie, but from timestamp 0 (FLV: from the first DTS,
#     which may be that of the audio). FLV without the duration metadata (as written to a
#     pipe), and Matroska whose duration is a length (as mkvmerge writes split parts).
#   * MP4 and MOV with B-frames and no edit list (fragmented MP4 with H.264 and HEVC,
#     -use_editlist 0), whose start time is the B-frame delay, and FLV without the duration
#     metadata whose audio ends less than a frame after the video.
#   * The bytes read at open: a 60 s Matroska with a start time, and FLV, also truncated.
#   * MOV/MP4 stream copies from a non-keyframe, and VFR MP4 with a one-frame gap, which have
#     fewer samples than frame times.
#   * MPEG-TS with two programs, MPEG-PS and VOB with B-frames, and an MPEG-PS cut in the
#     middle of a GOP (1 and 8 decoder threads): the end read from their last packets.
#   * NUT, whose duration is the start of the last packet (MPEG-4 with B-frames has no start time): FFV1,
#     also with a non-zero start time and with audio, MPEG-4, ProRes, and MPEG-4 and H.264 with B-frames.
#     NUT written without an index: FFV1, also with a non-zero start time, MPEG-4 and H.264 with B-frames.
#     NUT files larger than the 4 MB that the end probe reads (FFV1), with and without an index, also with a
#     non-zero start time and with a longer audio track, and a single GOP larger than that (H.264).
#   * An MPEG-TS whose end probe fails (its last 4 MB are audio).
#   * An MPEG-2 elementary stream with B-frames, which has no timestamps.
#   * MP4 with an edit list that does not display the first 1 or 3 frames, and MOV with one that does
#     not display the first 2 (negative timestamps): they are counted in the stream's number of frames,
#     but not displayed.
#   * MPEG-TS and M2TS whose 33-bit timestamps wrap during the clip (#189): libavformat gives the frames
#     before the wrap negative timestamps, and they must be counted and displayed (only MOV/MP4 edit lists
#     have a pre-roll that is not displayed).
#   * AVI (MPEG-4 with B-frames), which has no PTS.
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
#   * Damaged packets, which the decoder rejects (AVERROR_INVALIDDATA): H.264 in Matroska
#     (a damaged P-frame, IDR frame or B-frame; two damaged B-frames in a VFR clip), ProRes
#     in MOV and MJPEG in AVI (intra-only), with 1 and 8 decoder threads: the reader must
#     drop them, and their frame times show the previous frame. With a damaged first GOP,
#     its frames must fail with an error, and the other frames must decode. AV1 (libdav1d,
#     which reports damage as AVERROR(ENOMEM) with threads) with two scrambled frames.
#   * Truncated ProRes, MJPEG and v210 in MOV (faststart): the missing frames fail with an error,
#     sequentially and after a seek.
#   * Intra-only codecs found from their codec descriptor (v210, rawvideo, HuffYUV, JPEG-LS, JPEG 2000,
#     Hap) with damaged packets: the other frames must give the images of the clean file in every order.
#   * MPEG-TS with a continuity counter error (a packet flagged as corrupt, but not at the end
#     of the file): every frame must decode.
#   * Clean VP9 (WebM and Matroska, 8 and 10 bits, with and without alt-ref frames) and AV1, with 1, 4 and 8
#     decoder threads: every frame must be that of a lossless copy, in every order (issue #159).
#   * Forward requests (#311): H.264 in MP4, Matroska and MPEG-TS with a GOP of 50 frames, decoded in an order with
#     forward jumps in a GOP (decoded forward, without a seek), across a keyframe and backward (which seek); H.264
#     MPEG-TS with a 20 s timestamp jump, and MPEG-4 ASF without an index, where requests far ahead seek; HEVC MP4
#     with open GOPs, where requests close ahead decode forward, also leading pictures.
#   * MOV with broken timestamps (the PTS of a P-frame and a B-frame are swapped, so
#     that one frame time has two frames and another one none): every frame must
#     decode, and give the same image sequentially and after a seek, except at these
#     two frame times (see check_swapped).
#   * The Y'CbCr matrix and range of the conversion to RGB (see check_color): a constant Y'CbCr color in
#     lossless H.264 (tags in the VUI) and 10-bit FFV1 in Matroska (tags in the container), tagged bt709,
#     bt470bg, smpte170m, bt2020nc, fcc and smpte240m, in video and full range, and untagged (SD and 720p),
#     and the --matrix and --range overrides: the decoded RGB must be the one computed from the matrix and the range.
#   * The bit depth (see check_bitdepth, issue #213): a horizontal gray ramp with one code value per column in
#     10-bit VP9 (4:2:0 and 4:2:2), AV1, HEVC Main 10 and H.264 High 10, 12-bit VP9, 16-bit Bayer FFV1, and an 8-bit
#     VP9 control: the reader must report the bit depth of the decoded pixel format, and decode the ramp without
#     8-bit steps.
# Clips whose encoder is not available in the ffmpeg CLI are skipped.
#
# Environment:
#   FFMPEG_READ_TEST_KEEP=dir  generate the clips in dir and keep them
#   FFMPEG_READ_TEST_ARGS=...  extra harness arguments (e.g. --verbose)
#   FFMPEG_READ_TEST_BIN=path  the harness (default: ffmpeg_read_test next to this script, as tests/Makefile
#                              builds it; ctest sets it to the one of the CMake build)
#
# Exit status: 0 = all passed, non-zero = a failure was detected.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
bin="${FFMPEG_READ_TEST_BIN:-$here/ffmpeg_read_test}"
if [ ! -x "$bin" ]; then
    echo "error: $bin not built. Run 'make' (or build the CMake build's ffmpeg_read_test) first." >&2
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
    # Not `ffmpeg ... | grep -q`: grep -q exits at the first match, ffmpeg then gets SIGPIPE if it still
    # writes, and with pipefail the pipeline fails, which skipped the clips of that encoder at random.
    local help
    help="$(ffmpeg -hide_banner -h "encoder=$1" 2>/dev/null)" || return 1
    [[ "$help" == "Encoder $1 "* || "$help" == *$'\n'"Encoder $1 "* ]]
}
X264="-c:v libx264 -g 8 -bf 2 -pix_fmt yuv420p"

rc=0
# check_frames NAME DESCRIPTION ENCODER RATE EXPECTED_FRAMES FIRST_FRAME FFMPEG_OUTPUT_ARGS... [-- FFMPEG_INPUT_ARGS...]
# A gray ramp of $NFRAMES frames, of which the reader must report EXPECTED_FRAMES, the first one
# being ramp frame FIRST_FRAME (0-based), e.g. if an edit list does not display the first frames.
# FFMPEG_INPUT_ARGS, after "--", are extra ffmpeg inputs, before the ramp (e.g. an audio track:
# the ramp is then input 1).
check_frames() {
    local name="$1" desc="$2" encoder="$3" rate="$4" expected="$5" first="$6"
    shift 6
    echo "== $desc =="
    if ! have_encoder "$encoder"; then
        echo "SKIPPED: no $encoder encoder in the ffmpeg CLI"
        return
    fi
    local outargs=() inargs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do
        outargs+=("$1")
        shift
    done
    if [ $# -gt 0 ]; then
        shift
        inargs=("$@")
    fi
    if ! ffmpeg -y -loglevel error ${inargs[@]+"${inargs[@]}"} -f lavfi -i "$(ramp "$rate")" -frames:v "$NFRAMES" ${outargs[@]+"${outargs[@]}"} "$tmp/$name"; then
        echo "FAIL: could not generate $name"
        rc=1
        return
    fi
    # shellcheck disable=SC2086
    "$bin" "$tmp/$name" --monotonic --tol 3 --ramp "$((16 + 4 * first)),4" --frames "$expected" ${FFMPEG_READ_TEST_ARGS:-} || rc=1
}

# check NAME DESCRIPTION ENCODER RATE FFMPEG_OUTPUT_ARGS... [-- FFMPEG_INPUT_ARGS...]
check() {
    local name="$1" desc="$2" encoder="$3" rate="$4"
    shift 4
    check_frames "$name" "$desc" "$encoder" "$rate" "$NFRAMES" 0 "$@"
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

# check_audio NAME DESCRIPTION ENCODER AUDIO_SECONDS EXPECTED_FRAMES FFMPEG_OUTPUT_ARGS...
# A gray ramp of $NFRAMES frames at 24 fps with a silent audio track of AUDIO_SECONDS, of which the reader must
# report EXPECTED_FRAMES: with a longer audio track, the frames after the last one hold it (--positions).
check_audio() {
    local name="$1" desc="$2" encoder="$3" audio="$4" expected="$5"
    shift 5
    echo "== $desc =="
    if ! have_encoder "$encoder"; then
        echo "SKIPPED: no $encoder encoder in the ffmpeg CLI"
        return
    fi
    local positions="" i
    for ((i = 0; i < NFRAMES; i++)); do
        positions="${positions:+$positions,}$i"
    done
    # (a trim, not -frames:v, which would also end the audio)
    if ! ffmpeg -y -loglevel error -f lavfi -i "$(ramp 24)" -f lavfi -t "$audio" -i anullsrc=r=48000:cl=mono \
        -vf "trim=end_frame=$NFRAMES" "$@" "$tmp/$name"; then
        echo "FAIL: could not generate $name"
        rc=1
        return
    fi
    # shellcheck disable=SC2086
    "$bin" "$tmp/$name" --tol 3 --ramp 16,4 --positions "$positions" --frames "$expected" ${FFMPEG_READ_TEST_ARGS:-} || rc=1
}

# check_large_nut NAME DESCRIPTION AUDIO_SECONDS EXPECTED_FRAMES FFMPEG_OUTPUT_ARGS... [-- HARNESS_ARGS...]
# A 60-frame 24 fps 640x480 gray ramp with noise, in NUT (about 10 MB in FFV1 or lossless H.264: the last packets
# are more than 4 MB after the start, so the end must be found by a seek), with a silent audio track of
# AUDIO_SECONDS if it is not 0. The reader must report EXPECTED_FRAMES: with a longer audio track, the frames after
# the last one hold it (--positions). FFMPEG_OUTPUT_ARGS select the encoder (the first one is -c:v ENCODER), and
# HARNESS_ARGS are passed to the harness.
check_large_nut() {
    local name="$1" desc="$2" audio="$3" expected="$4" encoder="$6"
    shift 4
    echo "== $desc =="
    if ! have_encoder "$encoder"; then
        echo "SKIPPED: no $encoder encoder in the ffmpeg CLI"
        return
    fi
    local outargs=() harnessargs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do
        outargs+=("$1")
        shift
    done
    if [ $# -gt 0 ]; then
        shift
        harnessargs=("$@")
    fi
    local n=60 positions="" i
    for ((i = 0; i < n; i++)); do
        positions="${positions:+$positions,}$i"
    done
    local audioin=()
    if [ "$audio" != 0 ]; then
        audioin=(-f lavfi -t "$audio" -i anullsrc=r=48000:cl=mono -c:a pcm_s16le)
    fi
    # (a trim, not -frames:v, which would also end the audio)
    if ! ffmpeg -y -loglevel error -f lavfi -i "color=c=gray:s=640x480:r=24,format=gray,geq=lum='16+3*N+16*(random(1)-0.5)':cb=128:cr=128,format=yuv420p" \
        ${audioin[@]+"${audioin[@]}"} -vf "trim=end_frame=$n" "${outargs[@]}" "$tmp/$name"; then
        echo "FAIL: could not generate $name"
        rc=1
        return
    fi
    # shellcheck disable=SC2086
    "$bin" "$tmp/$name" --tol 3 --ramp 16,3 --positions "$positions" --frames "$expected" ${harnessargs[@]+"${harnessargs[@]}"} \
        ${FFMPEG_READ_TEST_ARGS:-} || rc=1
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

# check_mkv_length NAME DESCRIPTION FFMPEG_OUTPUT_ARGS...
# A 40-frame ramp in Matroska with a start time of 0.5 s (and a cluster every 100 ms), whose duration
# is then rewritten as a length (without the start time), as mkvmerge writes split parts: the
# duration is not the end of the movie, which must not be cut (with a long GOP, the cues, which only
# index keyframes, do not show it).
check_mkv_length() {
    local name="$1" desc="$2"
    shift 2
    echo "== $desc =="
    if ! have_encoder libx264; then
        echo "SKIPPED: no libx264 encoder in the ffmpeg CLI"
        return
    fi
    if ! command -v perl >/dev/null 2>&1; then
        echo "SKIPPED: no perl"
        return
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i "$(ramp 24)" -frames:v "$NFRAMES" -c:v libx264 -bf 2 -pix_fmt yuv420p "$@" -output_ts_offset 0.5 -cluster_time_limit 100 "$tmp/clean_$name"; then
        echo "FAIL: could not generate clean_$name"
        rc=1
        return
    fi
    # The segment duration is an 8-byte float (EBML ID 0x4489), in milliseconds: subtract the start time.
    if ! perl -e '
        local $/; open(my $f, "<:raw", $ARGV[0]) or die; my $d = <$f>; close($f);
        my $i = index($d, "\x44\x89\x88"); die "no 8-byte duration\n" if $i < 0;
        substr($d, $i + 3, 8) = pack("d>", unpack("d>", substr($d, $i + 3, 8)) - 500);
        open(my $o, ">:raw", $ARGV[1]) or die; print $o $d; close($o);' "$tmp/clean_$name" "$tmp/$name"; then
        echo "FAIL: could not rewrite the duration of clean_$name"
        rc=1
        return
    fi
    # shellcheck disable=SC2086
    "$bin" "$tmp/$name" --monotonic --tol 3 --ramp 16,4 --frames "$NFRAMES" ${FFMPEG_READ_TEST_ARGS:-} || rc=1
}

# check_flv_overhang NAME DESCRIPTION
# A 40-frame ramp in FLV without the duration metadata, with an AAC audio track that ends less than a
# frame after the last video frame: it must not add a frame.
check_flv_overhang() {
    local name="$1" desc="$2"
    echo "== $desc =="
    if ! have_encoder libx264 || ! have_encoder aac; then
        echo "SKIPPED: no libx264 or aac encoder in the ffmpeg CLI"
        return
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i "$(ramp 24)" -frames:v "$NFRAMES" $X264 "$tmp/video_$name.mkv" ||
        ! ffmpeg -y -loglevel error -f lavfi -t 1.70 -i anullsrc=r=48000:cl=mono -c:a aac "$tmp/audio_$name.m4a" ||
        ! ffmpeg -y -loglevel error -i "$tmp/video_$name.mkv" -i "$tmp/audio_$name.m4a" -map 0:v -map 1:a -c copy \
            -flvflags no_duration_filesize "$tmp/$name"; then
        echo "FAIL: could not generate $name"
        rc=1
        return
    fi
    # shellcheck disable=SC2086
    "$bin" "$tmp/$name" --monotonic --tol 3 --ramp 16,4 --frames "$NFRAMES" ${FFMPEG_READ_TEST_ARGS:-} || rc=1
}

# check_open NAME DESCRIPTION EXPECTED_FRAMES MAX_OPEN_BYTES [HARNESS_ARGS...]
# Opens the existing clip $tmp/NAME: the reader must report EXPECTED_FRAMES frames, and the open must read
# at most MAX_OPEN_BYTES bytes (e.g. finding the end of the movie must not read the whole file).
check_open() {
    local name="$1" desc="$2" expected="$3" maxbytes="$4"
    shift 4
    echo "== $desc =="
    # shellcheck disable=SC2086
    "$bin" "$tmp/$name" --frames "$expected" --max-open-bytes "$maxbytes" --open-only "$@" ${FFMPEG_READ_TEST_ARGS:-} || rc=1
}

# check_ss_copy NAME DESCRIPTION EXPECTED_FRAMES SEEK FFMPEG_OUTPUT_ARGS...
# A stream copy from SEEK seconds (not a keyframe) of a 100-frame 25 fps H.264 clip with B-frames and
# audio, for 1.6 s: the copy has a gap in its timestamps (fewer samples than frame times), and the last
# sample must be counted.
check_ss_copy() {
    local name="$1" desc="$2" expected="$3" seek="$4"
    shift 4
    echo "== $desc =="
    if ! have_encoder libx264 || ! have_encoder aac; then
        echo "SKIPPED: no libx264 or aac encoder in the ffmpeg CLI"
        return
    fi
    if [ ! -f "$tmp/ss_source.mp4" ] && ! ffmpeg -y -loglevel error -f lavfi -i testsrc2=size=160x120:rate=25 -f lavfi -i sine=sample_rate=48000 \
        -frames:v 100 -t 4 -c:v libx264 -preset veryfast -g 12 -bf 2 -c:a aac "$tmp/ss_source.mp4"; then
        echo "FAIL: could not generate ss_source.mp4"
        rc=1
        return
    fi
    if ! ffmpeg -y -loglevel error -ss "$seek" -i "$tmp/ss_source.mp4" -t 1.6 -c copy "$@" "$tmp/$name"; then
        echo "FAIL: could not generate $name"
        rc=1
        return
    fi
    # shellcheck disable=SC2086
    "$bin" "$tmp/$name" --tol 3 --frames "$expected" ${FFMPEG_READ_TEST_ARGS:-} || rc=1
}

# check_damaged NAME DESCRIPTION ENCODER SETPTS PACKETS GAPS CONCEALED FAIL FFMPEG_OUTPUT_ARGS...
# A 40-frame gray ramp at 24 fps, with the timestamps SETPTS (a setpts expression in frames, or N
# for CFR), where the packets PACKETS (0-based, in decoding order, e.g. "13 27") are damaged: the
# noise bitstream filter modifies each of their bytes, so that the decoder rejects them
# (AVERROR_INVALIDDATA). The reader must drop them and go on decoding, with 1 and with 8 decoder
# threads (frame threads):
#   * CFR: the frames GAPS (1-based, e.g. "14,28") of the damaged packets are gaps, which show
#     the previous frame, with its ramp content (--gaps). The frames CONCEALED (e.g. the GOP of a
#     damaged reference frame, and the frames before it, which the decoder may output late or not
#     at all) must decode, but their content may be anything that the decoder gives (--concealed).
#   * VFR (SETPTS is not N): the frames of the other packets are given by their timestamps
#     (--positions), and a frame in a gap must be identical to the frame displayed at that time.
#   * If FAIL (1-based, e.g. "1,2") is not empty, these frames cannot be decoded at all (e.g. the
#     first GOP is damaged), and must fail with an error (--fail).
# CHECK_DAMAGED_ARGS are extra harness arguments (e.g. --fresh F1,F2,...).
check_damaged() {
    local name="$1" desc="$2" encoder="$3" setpts="$4" packets="$5" gaps="$6" concealed="$7" fail="$8"
    shift 8
    echo "== $desc =="
    if ! have_encoder "$encoder"; then
        echo "SKIPPED: no $encoder encoder in the ffmpeg CLI"
        return
    fi
    if ! command -v ffprobe >/dev/null 2>&1; then
        echo "SKIPPED: no ffprobe CLI"
        return
    fi
    local vf=()
    if [ "$setpts" != N ]; then
        vf=(-vf "trim=end_frame=$NFRAMES,setpts=($setpts)/(24*TB)" -fps_mode passthrough)
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i "$(ramp 24)" -frames:v "$NFRAMES" ${vf[@]+"${vf[@]}"} "$@" "$tmp/clean_$name"; then
        echo "FAIL: could not generate clean_$name"
        rc=1
        return
    fi
    local expr="0" n
    for n in $packets; do
        expr="$expr+eq(n\,$n)"
    done
    if ! ffmpeg -y -loglevel error -i "$tmp/clean_$name" -c copy -bsf:v "noise=amount=if($expr\,1\,0)" "$tmp/$name"; then
        echo "FAIL: could not damage clean_$name"
        rc=1
        return
    fi
    local args=()
    if [ -n "$fail" ]; then
        args+=(--fail "$fail")
    fi
    if [ "$setpts" = N ]; then
        args+=(--ramp 16,4)
        if [ -n "$gaps" ]; then
            args+=(--gaps "$gaps")
        fi
    else
        # The frames (Matroska timestamps are in milliseconds) of the packets that are not damaged.
        args+=(--positions "$(ffprobe -v error -select_streams v:0 -show_entries packet=pts -of csv=p=0 "$tmp/clean_$name" |
            awk -v damaged=" $packets " 'index(damaged, " " (NR - 1) " ") == 0 { printf "%d\n", ($1 * 24 + 500) / 1000 }' | sort -n | paste -s -d, -)")
    fi
    if [ -n "$concealed" ]; then
        args+=(--concealed "$concealed")
    fi
    local threads
    for threads in 1 8; do
        # (the decoder errors are not shown, but the harness failures are)
        # shellcheck disable=SC2086
        if ! FFMPEG_READ_TEST_THREADS=$threads "$bin" "$tmp/$name" --tol 3 "${args[@]}" ${CHECK_DAMAGED_ARGS:-} ${FFMPEG_READ_TEST_ARGS:-} 2>"$tmp/$name.err"; then
            grep '^FAIL' "$tmp/$name.err" || echo "FAIL"
            echo "(with $threads decoder threads)"
            rc=1
        fi
    done
}

# check_damaged_av1 NAME DESCRIPTION ENCODER FFMPEG_OUTPUT_ARGS...
# A 40-frame ramp in AV1 (Matroska) where the second half of the data of packets 13 and 27 is scrambled
# (the muxers reject an AV1 packet damaged by the noise bitstream filter, whose OBU headers are broken):
# libdav1d reports the damage as AVERROR_INVALIDDATA, or with threads as AVERROR(ENOMEM). Every frame
# must decode, with 1 and 8 decoder threads (the content of the frames that reference a damaged one is
# not checked).
check_damaged_av1() {
    local name="$1" desc="$2" encoder="$3"
    shift 3
    echo "== $desc =="
    if ! have_encoder "$encoder"; then
        echo "SKIPPED: no $encoder encoder in the ffmpeg CLI"
        return
    fi
    local decoders
    decoders="$(ffmpeg -hide_banner -decoders 2>/dev/null || true)"
    if ! echo "$decoders" | grep -q " libdav1d "; then
        echo "SKIPPED: no libdav1d decoder"
        return
    fi
    if ! command -v ffprobe >/dev/null 2>&1 || ! command -v perl >/dev/null 2>&1; then
        echo "SKIPPED: no ffprobe CLI or no perl"
        return
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i "$(ramp 24)" -frames:v "$NFRAMES" "$@" "$tmp/clean_$name" 2>/dev/null; then
        echo "FAIL: could not generate clean_$name"
        rc=1
        return
    fi
    # ffprobe gives the position of the Matroska block, whose data starts 4 bytes later
    local ranges
    ranges=$(ffprobe -v error -select_streams v:0 -show_entries packet=pos,size -of csv=p=0 "$tmp/clean_$name" |
        awk -F, 'NR == 14 || NR == 28 { h = int($1 / 2); print $2 + 4 + h "," $1 - h }' | tr '\n' ' ')
    # shellcheck disable=SC2086
    if ! perl -e '
        local $/; open(my $f, "<:raw", shift) or die; my $d = <$f>; close($f); my $out = shift;
        for my $r (@ARGV) { my ($pos, $size) = split(/,/, $r);
            for (my $i = $pos; $i < $pos + $size; ++$i) { substr($d, $i, 1) = chr(((ord(substr($d, $i, 1)) ^ 0x5a) + 0x33 + $i) & 0xff); } }
        open(my $o, ">:raw", $out) or die; print $o $d; close($o);' "$tmp/clean_$name" "$tmp/$name" $ranges; then
        echo "FAIL: could not damage clean_$name"
        rc=1
        return
    fi
    local all="" i threads
    for ((i = 1; i <= NFRAMES; i++)); do
        all="${all:+$all,}$i"
    done
    for threads in 1 8; do
        # shellcheck disable=SC2086
        if ! FFMPEG_READ_TEST_THREADS=$threads "$bin" "$tmp/$name" --tol 3 --frames "$NFRAMES" --concealed "$all" ${FFMPEG_READ_TEST_ARGS:-} 2>"$tmp/$name.err"; then
            grep '^FAIL' "$tmp/$name.err" || echo "FAIL"
            echo "(with $threads decoder threads)"
            rc=1
        fi
    done
}

# check_truncated_intra NAME DESCRIPTION FIRST_FAIL ENCODER FFMPEG_OUTPUT_ARGS...
# A 48-frame ramp of an intra-only codec in MOV with the index at the start (faststart), truncated in
# the middle of the packet of frame 16, which the demuxer flags as corrupt: the frames 16 to 48 have no
# data, and must fail with an error, as for issue #134, without making the file invalid, sequentially
# and after a seek (in any render order: the seek reads no packet, and the reader decodes from earlier
# frames), and the frames before must decode, with 1 and 8 decoder threads. With a long-GOP codec, the
# frames fail from FIRST_FAIL on (the DTS of the truncated packet, found in the index at open, so that the
# result does not depend on the render order).
check_truncated_intra() {
    local name="$1" desc="$2" first="$3" encoder="$4"
    shift 4
    echo "== $desc =="
    if ! have_encoder "$encoder"; then
        echo "SKIPPED: no $encoder encoder in the ffmpeg CLI"
        return
    fi
    if ! command -v ffprobe >/dev/null 2>&1; then
        echo "SKIPPED: no ffprobe CLI"
        return
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i "$(ramp 24)" -frames:v 48 "$@" -movflags +faststart "$tmp/clean_$name"; then
        echo "FAIL: could not generate clean_$name"
        rc=1
        return
    fi
    local pos size
    read -r size pos < <(ffprobe -v error -select_streams v:0 -show_entries packet=pos,size -of csv=p=0 "$tmp/clean_$name" | sed -n 16p | tr ',' ' ')
    head -c $((pos + size / 2)) "$tmp/clean_$name" > "$tmp/$name"
    local fail="" i threads
    for ((i = first; i <= 48; i++)); do
        fail="${fail:+$fail,}$i"
    done
    for threads in 1 8; do
        # shellcheck disable=SC2086
        if ! FFMPEG_READ_TEST_THREADS=$threads "$bin" "$tmp/$name" --tol 3 --ramp 16,4 --frames 48 --fail "$fail" ${FFMPEG_READ_TEST_ARGS:-} 2>"$tmp/$name.err"; then
            grep '^FAIL' "$tmp/$name.err" || echo "FAIL"
            echo "(with $threads decoder threads)"
            rc=1
        fi
    done
}

# check_ts_cc NAME DESCRIPTION
# A 72-frame H.264 MPEG-TS with an MP2 audio track that ends 0.6 s after the video (so that the last frame
# is held until the end of the movie, 87 frames), and a continuity counter error in the packet of frame
# 18 (in decoding order), which the demuxer flags as corrupt: it is not a truncated file, and every frame
# must decode.
check_ts_cc() {
    local name="$1" desc="$2"
    echo "== $desc =="
    if ! have_encoder libx264 || ! have_encoder mp2; then
        echo "SKIPPED: no libx264 or mp2 encoder in the ffmpeg CLI"
        return
    fi
    if ! command -v perl >/dev/null 2>&1; then
        echo "SKIPPED: no perl"
        return
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i testsrc2=s=320x240:r=25 -frames:v 72 -c:v libx264 -g 12 -bf 2 -pix_fmt yuv420p "$tmp/video_$name.mkv" ||
        ! ffmpeg -y -loglevel error -f lavfi -t 3.5 -i sine=r=48000 -c:a mp2 "$tmp/audio_$name.mp2" ||
        ! ffmpeg -y -loglevel error -i "$tmp/video_$name.mkv" -i "$tmp/audio_$name.mp2" -map 0:v -map 1:a -c copy -f mpegts "$tmp/clean_$name"; then
        echo "FAIL: could not generate clean_$name"
        rc=1
        return
    fi
    # Change the continuity counter of the second TS packet of the 18th video PES.
    if ! perl -e '
        local $/; open(my $f, "<:raw", $ARGV[0]) or die; my $d = <$f>; close($f);
        my ($vpid, $pes, $done) = (-1, -1, 0);
        for (my $p = 0; $p + 188 <= length($d) && !$done; $p += 188) {
            next if ord(substr($d, $p, 1)) != 0x47;
            my $b1 = ord(substr($d, $p + 1, 1)); my $b3 = ord(substr($d, $p + 3, 1));
            my $pid = (($b1 & 0x1f) << 8) | ord(substr($d, $p + 2, 1)); my $afc = ($b3 >> 4) & 3;
            next unless $afc & 1;
            my $off = $p + 4; $off += 1 + ord(substr($d, $off, 1)) if $afc & 2;
            if (($b1 & 0x40) && substr($d, $off, 3) eq "\x00\x00\x01") {
                my $sid = ord(substr($d, $off + 3, 1));
                $vpid = $pid if $vpid < 0 && $sid >= 0xe0 && $sid <= 0xef;
                ++$pes if $pid == $vpid;
            } elsif ($pid == $vpid && $pes == 17) {
                substr($d, $p + 3, 1) = chr(($b3 & 0xf0) | ((($b3 & 0x0f) + 5) & 0x0f)); $done = 1;
            }
        }
        die "no TS packet changed\n" unless $done;
        open(my $o, ">:raw", $ARGV[1]) or die; print $o $d; close($o);' "$tmp/clean_$name" "$tmp/$name"; then
        echo "FAIL: could not damage clean_$name"
        rc=1
        return
    fi
    local threads
    for threads in 1 8; do
        # shellcheck disable=SC2086
        if ! FFMPEG_READ_TEST_THREADS=$threads "$bin" "$tmp/$name" --tol 3 --frames 87 ${FFMPEG_READ_TEST_ARGS:-} 2>"$tmp/$name.err"; then
            grep '^FAIL' "$tmp/$name.err" || echo "FAIL"
            echo "(with $threads decoder threads)"
            rc=1
        fi
    done
}

# check_damaged_long NAME DESCRIPTION NOISE_AMOUNT FAIL_FROM FAIL_TO FRESH MAX_SEQ_BYTES_FACTOR [ORDER]
# A 300-frame 320x240 ProRes MOV, damaged by the noise bitstream filter with the amount expression
# NOISE_AMOUNT (e.g. "between(n,100,199)", a run of 100 damaged packets, or "gte(n,6)", damaged from the 7th
# packet on): the frames FAIL_FROM to FAIL_TO (1-based) must fail, without making the file invalid, sequentially,
# after a seek, and when decoded first in a newly opened file (FRESH, e.g. the good frames just after the
# damage, which a damaged range must not include), with 1 and 8 decoder threads; the other frames must decode.
# Decoding every frame sequentially, and in reverse order, must read at most MAX_SEQ_BYTES_FACTOR times the
# size of the file (the known damaged ranges are extended, instead of reading the damaged packets again for
# each frame). If ORDER is given (e.g. "101,184,181"), these frames are also decoded in that order in a newly
# opened file.
check_damaged_long() {
    local name="$1" desc="$2" amount="$3" from="$4" to="$5" fresh="$6" factor="$7" order="${8:-}"
    echo "== $desc =="
    if ! have_encoder prores_ks; then
        echo "SKIPPED: no prores_ks encoder in the ffmpeg CLI"
        return
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i testsrc2=s=320x240:r=25:d=12 -c:v prores_ks -profile:v 2 "$tmp/clean_$name" ||
        ! ffmpeg -y -loglevel error -i "$tmp/clean_$name" -c copy -bsf:v "noise=amount='$amount'" "$tmp/$name"; then
        echo "FAIL: could not generate $name"
        rc=1
        return
    fi
    local fail="" i threads size
    for ((i = from; i <= to; i++)); do
        fail="${fail:+$fail,}$i"
    done
    size=$(wc -c < "$tmp/$name")
    for threads in 1 8; do
        # shellcheck disable=SC2086
        if ! FFMPEG_READ_TEST_THREADS=$threads "$bin" "$tmp/$name" --tol 3 --frames 300 --fail "$fail" ${fresh:+--fresh "$fresh"} \
            ${order:+--order "$order"} --max-seq-bytes $((size * factor)) --max-reverse-bytes $((size * factor)) ${FFMPEG_READ_TEST_ARGS:-} 2>"$tmp/$name.err"; then
            grep '^FAIL' "$tmp/$name.err" || echo "FAIL"
            echo "(with $threads decoder threads)"
            rc=1
        fi
    done
}

# check_damaged_gop NAME DESCRIPTION FRAMES NOISE_AMOUNT FAIL_FROM FAIL_TO MAX_SEQ_BYTES_FACTOR FFMPEG_OUTPUT_ARGS...
# A long-GOP H.264 clip (testsrc2, 25 fps, FRAMES frames) whose packets are damaged by the noise bitstream
# filter (NOISE_AMOUNT, an expression of the packet number n): the frames FAIL_FROM to FAIL_TO (1-based)
# must fail, and every other frame must decode, sequentially and after a seek, with 1 and 8 decoder
# threads; if MAX_SEQ_BYTES_FACTOR is not empty, decoding every frame sequentially, and in reverse order,
# must read at most MAX_SEQ_BYTES_FACTOR times the file (the frames that cannot be decoded from the start of
# the file are not decoded again).
check_damaged_gop() {
    local name="$1" desc="$2" frames="$3" amount="$4" from="$5" to="$6" factor="$7"
    shift 7
    echo "== $desc =="
    if ! have_encoder libx264; then
        echo "SKIPPED: no libx264 encoder in the ffmpeg CLI"
        return
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i testsrc2=s=160x120:r=25 -frames:v "$frames" -c:v libx264 -pix_fmt yuv420p "$@" "$tmp/clean_$name" ||
        ! ffmpeg -y -loglevel error -i "$tmp/clean_$name" -c copy -bsf:v "noise=amount='$amount'" "$tmp/$name"; then
        echo "FAIL: could not generate $name"
        rc=1
        return
    fi
    local fail="" i threads bytes=()
    for ((i = from; i <= to; i++)); do
        fail="${fail:+$fail,}$i"
    done
    if [ -n "$factor" ]; then
        bytes=(--max-seq-bytes $(($(wc -c < "$tmp/$name") * factor)) --max-reverse-bytes $(($(wc -c < "$tmp/$name") * factor)))
    fi
    for threads in 1 8; do
        # shellcheck disable=SC2086
        if ! FFMPEG_READ_TEST_THREADS=$threads "$bin" "$tmp/$name" --tol 3 --frames "$frames" ${fail:+--fail "$fail"} \
            ${bytes[@]+"${bytes[@]}"} ${FFMPEG_READ_TEST_ARGS:-} 2>"$tmp/$name.err"; then
            grep '^FAIL' "$tmp/$name.err" || echo "FAIL"
            echo "(with $threads decoder threads)"
            rc=1
        fi
    done
}

# check_damaged_mkv_run NAME DESCRIPTION FROM TO REFERENCE_FRAMES [CONTAINER [DAMAGE]]
# A 12 s H.264 Matroska clip (testsrc2, 320x240, 25 fps, GOP 100 with B-frames: keyframes at packets 0, 100 and
# 200) where 200 bytes at offset 8 of the video packets FROM to TO (0-based, in decoding order) are overwritten
# with random bytes (perl, seeded): the damage also hits the Matroska block headers (e.g. keyframe flags on
# damaged packets, which the demuxer adds to the index), and damaged packets are parsed as H.264 parameter sets.
# The 1-based REFERENCE_FRAMES (e.g. "1-48,101-300"), which do not depend on the damaged
# packets, must give exactly the images of the clean file, sequentially, in reverse order, in a random order,
# with frame 120 then frame 1 in a newly opened file, and each frame in its own newly opened file, with 1 and 8
# decoder threads (--reference): these are the frames that the reader decoded before #158 (it failed at the
# first damaged packet), which must not fail or change. CONTAINER is mkv (default), pipe (a stream copy written to
# a pipe: Matroska without cues, whose index is only built from the blocks read, including the damaged ones), or
# mp4 (a stream copy). DAMAGE is data (default: 200 bytes at offset 8 of each packet, seed 4) or flags (only the
# flags byte of the Matroska SimpleBlock header of each packet, seed 2: some values turn on lacing, and the demuxer
# then splits or merges the blocks).
check_damaged_mkv_run() {
    local name="$1" desc="$2" from="$3" to="$4" frames="$5" container="${6:-mkv}" damage="${7:-data}" clean
    echo "== $desc =="
    if ! have_encoder libx264; then
        echo "SKIPPED: no libx264 encoder in the ffmpeg CLI"
        return
    fi
    if ! command -v perl >/dev/null 2>&1 || ! command -v ffprobe >/dev/null 2>&1; then
        echo "SKIPPED: no perl or ffprobe"
        return
    fi
    if [ ! -f "$tmp/clean_g100.mkv" ] &&
        ! ffmpeg -y -loglevel error -f lavfi -i testsrc2=s=320x240:r=25,format=yuv420p -t 12 -threads 4 -c:v libx264 -g 100 -keyint_min 100 \
            -sc_threshold 0 -bf 2 -b:v 600k "$tmp/clean_g100.mkv"; then
        echo "FAIL: could not generate clean_g100.mkv"
        rc=1
        return
    fi
    clean="$tmp/clean_g100.mkv"
    if [ "$container" = pipe ]; then
        clean="$tmp/clean_g100_pipe.mkv"
        [ -f "$clean" ] || ffmpeg -y -loglevel error -i "$tmp/clean_g100.mkv" -c copy -f matroska pipe:1 > "$clean" || rc=1
    elif [ "$container" = mp4 ]; then
        clean="$tmp/clean_g100.mp4"
        [ -f "$clean" ] || ffmpeg -y -loglevel error -i "$tmp/clean_g100.mkv" -c copy "$clean" || rc=1
    fi
    cp "$clean" "$tmp/$name"
    ffprobe -v error -select_streams v:0 -show_entries packet=pos,size -of compact=p=0 "$clean" > "$tmp/$name.packets"
    if ! perl -e '
        my ($file, $list, $from, $to, $damage) = @ARGV;
        srand($damage eq "flags" ? 2 : 4);
        open(my $l, "<", $list) or die;
        my @p;
        while (<$l>) { my ($size) = /size=(\d+)/; my ($pos) = /pos=(\d+)/; push @p, [$pos, $size]; }
        open(my $f, "+<:raw", $file) or die;
        for my $i ($from .. $to) {
            my ($pos, $size) = @{$p[$i]};
            if ($damage eq "flags") {
                # the block data starts with the track number (1 byte), the timecode (2 bytes) and the flags
                seek($f, $pos + 3, 0) or die;
                print $f pack("C", int(rand(256)));
                next;
            }
            my $n = $size - 8 < 200 ? $size - 8 : 200;
            next if $n <= 0;
            seek($f, $pos + 8, 0) or die;
            print $f pack("C*", map { int(rand(256)) } 1 .. $n);
        }
        close($f) or die;' "$tmp/$name" "$tmp/$name.packets" "$from" "$to" "$damage"; then
        echo "FAIL: could not damage $name"
        rc=1
        return
    fi
    local list="" range a b i threads
    for range in ${frames//,/ }; do
        a=${range%-*}
        b=${range#*-}
        for ((i = a; i <= b; i++)); do
            list="${list:+$list,}$i"
        done
    done
    for threads in 1 8; do
        # shellcheck disable=SC2086
        if ! FFMPEG_READ_TEST_THREADS=$threads "$bin" "$tmp/$name" --reference "$clean" --reference-frames "$list" \
            --order 120,1 ${FFMPEG_READ_TEST_ARGS:-} 2>"$tmp/$name.err"; then
            grep '^FAIL' "$tmp/$name.err" || echo "FAIL"
            echo "(with $threads decoder threads)"
            rc=1
        fi
    done
}

# check_damaged_ref NAME DESCRIPTION ENCODER FRAMES NOISE_AMOUNT REFERENCE_FRAMES FFMPEG_OUTPUT_ARGS...
# A FRAMES-frame clip (testsrc2, 320x240, 24 fps, ENCODER) damaged by the noise bitstream filter (NOISE_AMOUNT):
# the 1-based REFERENCE_FRAMES (e.g. "1-18") must give exactly the images of the clean file in every order, with 1 and
# 8 decoder threads (--reference).
check_damaged_ref() {
    local name="$1" desc="$2" encoder="$3" frames="$4" amount="$5" refs="$6" list="" range a b i threads
    shift 6
    echo "== $desc =="
    if ! have_encoder "$encoder"; then
        echo "SKIPPED: no $encoder encoder in the ffmpeg CLI"
        return
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i testsrc2=s=320x240:r=24 -frames:v "$frames" -c:v "$encoder" -pix_fmt yuv420p "$@" "$tmp/clean_$name" ||
        ! ffmpeg -y -loglevel error -i "$tmp/clean_$name" -c copy -bsf:v "noise=amount='$amount'" "$tmp/$name"; then
        echo "FAIL: could not generate $name"
        rc=1
        return
    fi
    for range in ${refs//,/ }; do
        a=${range%-*}
        b=${range#*-}
        for ((i = a; i <= b; i++)); do
            list="${list:+$list,}$i"
        done
    done
    for threads in 1 8; do
        # shellcheck disable=SC2086
        if ! FFMPEG_READ_TEST_THREADS=$threads "$bin" "$tmp/$name" --reference "$tmp/clean_$name" --reference-frames "$list" \
            ${FFMPEG_READ_TEST_ARGS:-} 2>"$tmp/$name.err"; then
            grep '^FAIL' "$tmp/$name.err" || echo "FAIL"
            echo "(with $threads decoder threads)"
            rc=1
        fi
    done
}

# check_frame_threads NAME DESCRIPTION ENCODER PIX_FMT FFMPEG_OUTPUT_ARGS...
# A clean 48-frame clip (testsrc with a moving luma offset, 256x160, 24 fps, GOP 12, ENCODER in 2 passes, so that
# libvpx makes alt-ref frames, which it only makes in 2 passes) must give exactly the images of its lossless copy
# (decoded by the ffmpeg CLI with 1 thread), in every order, with 1, 4 and 8 decoder threads (--reference). Issue #159:
# with frame threads, the VP9 decoder refuses a packet with AVERROR(EAGAIN) after a superframe (a hidden alt-ref frame
# and a shown frame), and the reader lost that packet when the drain gave the requested frame: sequentially, the
# frames after it decoded wrong until the next keyframe.
check_frame_threads() {
    local name="$1" desc="$2" encoder="$3" pixfmt="$4" all threads hidden ref
    shift 4
    # The lossless copy: FFV1, or for more than 8 bits lossless VP9 without alt-ref frames, which the reader decodes
    # through the same VP9 decoder and pixel format.
    local refargs=(-c:v ffv1)
    ref="$tmp/ref_$name.mkv"
    if [[ "$pixfmt" == *10* ]]; then
        refargs=(-c:v libvpx-vp9 -lossless 1 -auto-alt-ref 0)
        ref="$tmp/ref_$name.webm"
    fi
    echo "== $desc =="
    if ! have_encoder "$encoder"; then
        echo "SKIPPED: no $encoder encoder in the ffmpeg CLI"
        return
    fi
    # 48 frames from the source duration (d=2), not -frames:v 48: with an output frame limit, the ffmpeg 6.1 CLI
    # flushes the encoder twice (when the limit is reached, and again in enc_flush()), and writes the pass-1 stats
    # twice, so libvpx rejects pass 2 ("rc_twopass_stats_in missing EOS stats packet").
    local src="testsrc=s=256x160:r=24:d=2,format=yuv420p,geq=lum='lum(X,Y)+2*N':cb='cb(X,Y)':cr='cr(X,Y)',format=$pixfmt"
    if ! ffmpeg -y -loglevel error -f lavfi -i "$src" -c:v "$encoder" -g 12 "$@" -pass 1 -passlogfile "$tmp/$name.pass" -f null - ||
        ! ffmpeg -y -loglevel error -f lavfi -i "$src" -c:v "$encoder" -g 12 "$@" -pass 2 -passlogfile "$tmp/$name.pass" "$tmp/$name"; then
        echo "SKIPPED: the ffmpeg CLI could not encode $name with $encoder in 2 passes"
        return
    fi
    if ! ffmpeg -y -loglevel error -threads 1 -i "$tmp/$name" "${refargs[@]}" "$ref"; then
        echo "SKIPPED: the ffmpeg CLI could not make the lossless copy of $name (${refargs[*]})"
        return
    fi
    if [ "$encoder" = libvpx-vp9 ]; then
        # the hidden frames: the packets after splitting the superframes, minus the 48 packets
        hidden=$(ffmpeg -loglevel error -i "$tmp/$name" -c:v copy -bsf:v vp9_superframe_split -f framecrc - | grep -vc '^#') || true
        echo "  $((hidden - 48)) hidden (alt-ref) frames"
    fi
    all="$(seq -s, 1 48)"
    for threads in 1 4 8; do
        # shellcheck disable=SC2086
        if ! FFMPEG_READ_TEST_THREADS=$threads "$bin" "$tmp/$name" --reference "$ref" --reference-frames "$all" \
            ${FFMPEG_READ_TEST_ARGS:-} >"$tmp/$name.out" 2>&1; then
            grep -E '^  REFERENCE|^FAIL' "$tmp/$name.out" | head -5 || echo "FAIL"
            echo "(with $threads decoder threads)"
            rc=1
        else
            echo "  $threads decoder threads: PASS"
        fi
    done
}

# check_ts_wrap NAME DESCRIPTION RATE OFFSET FFMPEG_OUTPUT_ARGS...
# A 150-frame clip (testsrc2, 320x240, at RATE fps, H.264 with B-frames, and a sine audio track in AC3) in MPEG-TS
# (FFMPEG_OUTPUT_ARGS, e.g. -mpegts_m2ts_mode 1 for M2TS), stream-copied with -output_ts_offset OFFSET (in seconds),
# so that its 33-bit timestamps wrap (at 2^33 / 90000 = 95443.7 s) during the clip: libavformat then makes the
# timestamps before the wrap negative, and the start time too (update_wrap_reference(), libavformat/demux.c). Every
# frame must be counted (#189: they were counted from the first frame after the wrap, and the frames before it were
# lost), and give exactly the image of the clean file (before the stream copy), in every order, with 1 and 8 decoder
# threads (--reference).
check_ts_wrap() {
    local name="$1" desc="$2" rate="$3" offset="$4" list start threads
    shift 4
    echo "== $desc =="
    if ! have_encoder libx264 || ! have_encoder ac3; then
        echo "SKIPPED: no libx264 or ac3 encoder in the ffmpeg CLI"
        return
    fi
    if ! command -v ffprobe >/dev/null 2>&1; then
        echo "SKIPPED: no ffprobe CLI"
        return
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i "testsrc2=s=320x240:r=$rate" -f lavfi -i sine=r=48000 -frames:v 150 \
        -c:v libx264 -g 25 -bf 2 -pix_fmt yuv420p -c:a ac3 -shortest -f mpegts "$@" "$tmp/clean_$name" ||
        ! ffmpeg -y -loglevel error -i "$tmp/clean_$name" -map 0 -c copy -output_ts_offset "$offset" -f mpegts "$@" "$tmp/$name"; then
        echo "FAIL: could not generate $name"
        rc=1
        return
    fi
    # the start time must be negative, i.e. the timestamps wrap in the clip
    start="$(ffprobe -v error -select_streams v:0 -show_entries stream=start_time -of csv=p=0 "$tmp/$name")"
    if [[ "$start" != -* ]]; then
        echo "FAIL: $name: the video start time is $start, the timestamps do not wrap"
        rc=1
        return
    fi
    list="$(seq -s, 1 150)"
    for threads in 1 8; do
        # shellcheck disable=SC2086
        if ! FFMPEG_READ_TEST_THREADS=$threads "$bin" "$tmp/$name" --frames 150 --reference "$tmp/clean_$name" --reference-frames "$list" \
            ${FFMPEG_READ_TEST_ARGS:-} 2>"$tmp/$name.err"; then
            grep '^FAIL' "$tmp/$name.err" || echo "FAIL"
            echo "(with $threads decoder threads)"
            rc=1
        fi
    done
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

# Forward requests (#311): a forward request within Stream::forwardWindow() (the largest keyframe interval seen, 1 s
# to 10 s) decodes forward, without a seek, unless a keyframe comes between (from the index, if it can be trusted);
# the others seek. Each check decodes ORDER (1-based frames) in a newly opened file, and checks that the requests
# of the frames SEEK seek, that those of NO_SEEK (may be empty) do not, and that every frame is the sequential one
# (with 1 and 8 decoder threads).
# run_forward NAME ORDER SEEK NO_SEEK HARNESS_ARGS...
run_forward() {
    local name="$1" order="$2" seek="$3" noseek="$4" threads
    shift 4
    for threads in 1 8; do
        # shellcheck disable=SC2086
        if ! FFMPEG_READ_TEST_THREADS=$threads "$bin" "$tmp/$name" --order "$order" --order-seek "$seek" ${noseek:+--order-no-seek "$noseek"} \
            "$@" ${FFMPEG_READ_TEST_ARGS:-} >"$tmp/$name.out" 2>/dev/null; then
            grep -E '^  (ORDER|MISMATCH|CONTENT|OUT-OF-ORDER)' "$tmp/$name.out" | head -5
            echo "FAIL (with $threads decoder threads)"
            rc=1
            return
        fi
    done
    echo "PASS"
}

# check_forward NAME DESCRIPTION FFMPEG_OUTPUT_ARGS...
# A 100-frame lossless gray ramp (luma 16 + 2 * N) in H.264 with B-frames and a GOP of 50 frames (keyframes at
# the 1-based frames 1 and 51), at 25 fps, decoded in the order 10, 12, 15, 60, 55: 12 and 15 are in the GOP of
# the decoder position and decode forward, 60 is after the keyframe 51 (MP4 and Matroska: from the index;
# MPEG-TS, which has no index to trust: 45 frames is more than the window, 1 s here) and 55 is backward: they seek.
# Then 30, 40, 45: 40 and 45 are in the GOP of the decoder position, and decode forward (before the keyframe
# interval is known).
check_forward() {
    local name="$1" desc="$2"
    shift 2
    echo "== $desc =="
    if ! have_encoder libx264; then
        echo "SKIPPED: no libx264 encoder in the ffmpeg CLI"
        return
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i "color=c=gray:s=128x128:r=25,format=gray,geq=lum='16+2*N':cb=128:cr=128,format=yuv420p" \
        -frames:v 100 -c:v libx264 -qp 0 -g 50 -keyint_min 50 -bf 2 -x264-params scenecut=0 -pix_fmt yuv420p "$@" "$tmp/$name"; then
        echo "FAIL: could not generate $name"
        rc=1
        return
    fi
    run_forward "$name" 10,12,15,60,55 60,55 12,15 --tol 1 --monotonic --frames 100
    run_forward "$name" 30,40,45 30 40,45 --tol 1 --monotonic --frames 100
}

# check_forward_ts_jump NAME DESCRIPTION
# H.264 in MPEG-TS (25 fps, GOP 50, 400 frames) whose timestamps jump by 20 s after the 101st frame (the 1-based
# frames 102 to 601 are a gap): the keyframe interval is counted in packets, not made 550 frames long by the jump,
# so the requests after 101 (650, 800 and 880, more than the window ahead) seek.
check_forward_ts_jump() {
    local name="$1" desc="$2"
    echo "== $desc =="
    if ! have_encoder libx264; then
        echo "SKIPPED: no libx264 encoder in the ffmpeg CLI"
        return
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i testsrc2=s=128x96:r=25 -frames:v 400 -vf "setpts=if(lte(N\,100)\,N\,N+500)/(25*TB)" \
        -fps_mode passthrough -c:v libx264 -g 50 -bf 2 -pix_fmt yuv420p -f mpegts "$tmp/$name"; then
        echo "FAIL: could not generate $name"
        rc=1
        return
    fi
    run_forward "$name" 99,100,101,650,800,880 99,650,800,880 "" --tol 3 --frames 900
}

# check_forward_asf_noindex NAME DESCRIPTION
# MPEG-4 Part 2 in ASF (25 fps, GOP 25, 600 frames) whose simple index object, at the end of the file, is removed:
# the seeks are binary searches, which add the packets they find to the index (asf_read_pts(), asfdec_f.c:1486 in
# FFmpeg 9.0.2), so that it has holes. After 550 and 1, 2 decodes forward, and 250 (far ahead) seeks.
check_forward_asf_noindex() {
    local name="$1" desc="$2"
    echo "== $desc =="
    if ! have_encoder mpeg4; then
        echo "SKIPPED: no mpeg4 encoder in the ffmpeg CLI"
        return
    fi
    if ! command -v perl >/dev/null 2>&1; then
        echo "SKIPPED: no perl"
        return
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i testsrc2=s=128x96:r=25 -frames:v 600 -c:v mpeg4 -g 25 -q:v 5 "$tmp/clean_$name"; then
        echo "FAIL: could not generate clean_$name"
        rc=1
        return
    fi
    # The simple index object (ff_asf_simple_index_header, libavformat/asf_tags.c:90-92) and what follows it.
    if ! perl -e '
        local $/; open(my $f, "<:raw", $ARGV[0]) or die; my $d = <$f>; close($f);
        my $i = rindex($d, pack("C*", 0x90, 0x08, 0x00, 0x33, 0xB1, 0xE5, 0xCF, 0x11, 0x89, 0xF4, 0x00, 0xA0, 0xC9, 0x03, 0x49, 0xCB));
        die "no simple index object\n" if $i < 0;
        open(my $o, ">:raw", $ARGV[1]) or die; print $o substr($d, 0, $i); close($o);' "$tmp/clean_$name" "$tmp/$name"; then
        echo "FAIL: could not remove the index of clean_$name"
        rc=1
        return
    fi
    run_forward "$name" 550,1,2,250 550,1,250 2 --tol 3 --frames 600
}

# check_forward_open_gop NAME DESCRIPTION
# HEVC in MP4 with open GOPs (x265 open-gop, 24 fps, GOP 24, 3 B-frames: the CRA at the 1-based frame 25 is
# decoded before the leading pictures 22 to 24, which are displayed before it), and an index on the DTS: the
# requests close after the held frame decode forward, also the leading pictures (22, 23, 24) and the frame before
# them (21, whose PTS is after the DTS of the CRA), and every frame is the sequential one.
check_forward_open_gop() {
    local name="$1" desc="$2"
    echo "== $desc =="
    if ! have_encoder libx265; then
        echo "SKIPPED: no libx265 encoder in the ffmpeg CLI"
        return
    fi
    if ! ffmpeg -y -loglevel error -f lavfi -i testsrc2=s=128x96:r=24 -frames:v 96 -c:v libx265 \
        -x265-params "keyint=24:min-keyint=24:scenecut=0:open-gop=1:bframes=3:log-level=error" -pix_fmt yuv420p "$tmp/$name"; then
        echo "FAIL: could not generate $name"
        rc=1
        return
    fi
    run_forward "$name" 14,21,23,18,22,24 14,18 21,23,22,24 --tol 1 --frames 96
}

echo "Generating test clips in $tmp and checking them..."
check ramp.mp4 "MP4 (H.264, start_time 0)" libx264 25 $X264
check ramp_offset.mp4 "MP4 (H.264, 24 fps, non-zero start time)" libx264 24 $X264 -output_ts_offset 10
check ramp.mov "MOV (H.264, 24 fps)" libx264 24 $X264
check ramp.ts "MPEG-TS (H.264, non-zero start_time)" libx264 25 $X264 -f mpegts
# The audio track starts before the video: the frames are counted from the start of the video.
check ramp_avchd.m2ts "M2TS (AVCHD-like: H.264 interlaced, 29.97 fps, AC3 audio starting before the video)" libx264 30000/1001 \
    -map 1:v -map 0:a $X264 -flags +ildct+ilme -x264-params tff=1 -c:a ac3 -shortest -f mpegts -mpegts_m2ts_mode 1 \
    -- -f lavfi -i anullsrc=r=48000:cl=stereo
check ramp_23.976.mkv "Matroska (H.264, 23.976 fps)" libx264 24000/1001 $X264
check ramp_24.mkv "Matroska (H.264, 24 fps)" libx264 24 $X264
check ramp_25.mkv "Matroska (H.264, 25 fps)" libx264 25 $X264
check ramp_30.mkv "Matroska (H.264, 30 fps)" libx264 30 $X264
check ramp_60.mkv "Matroska (H.264, 60 fps)" libx264 60 $X264
# The Matroska and FLV durations are not counted from the start of the movie.
check ramp_offset.mkv "Matroska (H.264, 24 fps, non-zero start time)" libx264 24 $X264 -output_ts_offset 10.01
check ramp.flv "FLV (H.264 with B-frames, 24 fps)" libx264 24 $X264
check ramp_offset.flv "FLV (H.264 with B-frames, 24 fps, non-zero start time)" libx264 24 $X264 -output_ts_offset 10
# The audio starts first: the FLV duration is counted from its first DTS.
check ramp_audio_first.flv "FLV (H.264 with B-frames, 24 fps, starting 0.5 s after the AAC audio)" libx264 24 \
    -map 1:v -map 0:a $X264 -c:a aac -shortest -- -f lavfi -t 3 -i anullsrc=r=48000:cl=mono -itsoffset 0.5
# Without the duration metadata (as when written to a pipe), the demuxer gives the DTS of the last tag.
check ramp_nodur.flv "FLV (H.264 with B-frames, 24 fps, no duration metadata)" libx264 24 $X264 -flvflags no_duration_filesize
check ramp_nodur_offset.flv "FLV (H.264 with B-frames, 24 fps, no duration metadata, non-zero start time)" libx264 24 \
    $X264 -flvflags no_duration_filesize -output_ts_offset 10
# A low frame rate with audio: the last 64 tags are audio tags and the AVC end of sequence (not a frame).
check ramp_low_rate.flv "FLV (H.264, 0.5 fps, AAC audio)" libx264 0.5 \
    -map 1:v -map 0:a -c:v libx264 -bf 0 -pix_fmt yuv420p -c:a aac -shortest -- -f lavfi -t 80 -i sine=r=48000
check_mkv_length ramp_length.mkv "Matroska (H.264, 24 fps, GOP 8, start time 0.5 s, duration written as a length, as mkvmerge does)" -g 8
check_mkv_length ramp_length_gop.mkv "Matroska (H.264, 24 fps, default GOP, start time 0.5 s, duration written as a length)"
check_flv_overhang ramp_overhang.flv "FLV (H.264 with B-frames, 24 fps, no duration metadata, AAC audio ending 40 ms after the video)"
# MPEG-TS and M2TS whose 33-bit timestamps wrap during the clip (#189): the frames before the wrap have negative
# timestamps, and must be counted and displayed.
check_ts_wrap ts_pcrwrap.ts "MPEG-TS (H.264 with B-frames, 25 fps, the timestamps wrap during the clip)" 25 95440
check_ts_wrap m2ts_wrap.m2ts "M2TS (H.264 with B-frames, 29.97 fps, the timestamps wrap during the clip)" 30000/1001 95441 -mpegts_m2ts_mode 1
# MP4/MOV with B-frames and no edit list: the start time is the B-frame delay.
check ramp_frag.mp4 "Fragmented MP4 (H.264 with B-frames, no edit list)" libx264 24 $X264 -movflags frag_keyframe+empty_moov
check ramp_noeditlist.mov "MOV (H.264 with B-frames, no edit list)" libx264 24 $X264 -use_editlist 0
check ramp_hevc_frag.mp4 "Fragmented MP4 (HEVC with B-frames, no edit list)" libx265 24 \
    -c:v libx265 -x265-params log-level=error:bframes=3:keyint=8 -tag:v hvc1 -pix_fmt yuv420p -movflags frag_keyframe+empty_moov
# NUT stores the largest PTS of any stream as its duration, the start of the last packet, not its end (#215).
# With MPEG-4 B-frames the start time is unknown; with H.264 B-frames it is the B-frame delay (2 frames).
check ramp_mpeg4_bf.nut "NUT (MPEG-4 with B-frames)" mpeg4 24 -c:v mpeg4 -bf 2 -g 8 -q:v 2
check ramp_ffv1.nut "NUT (FFV1)" ffv1 24 -c:v ffv1
check ramp_ffv1_offset.nut "NUT (FFV1, non-zero start time)" ffv1 24 -c:v ffv1 -output_ts_offset 10
check ramp_mpeg4.nut "NUT (MPEG-4, no B-frames)" mpeg4 24 -c:v mpeg4 -bf 0 -g 8 -q:v 2
check ramp_prores.nut "NUT (ProRes)" prores_ks 24 -c:v prores_ks
check ramp_h264_bf.nut "NUT (H.264 with B-frames)" libx264 24 $X264
# Without an index (#331), the NUT duration is the PTS of the last syncpoint, several frames before the end.
check ramp_ffv1_noidx.nut "NUT without an index (FFV1)" ffv1 24 -c:v ffv1 -write_index 0
check ramp_ffv1_offset_noidx.nut "NUT without an index (FFV1, non-zero start time)" ffv1 24 -c:v ffv1 -output_ts_offset 10 -write_index 0
check ramp_mpeg4_bf_noidx.nut "NUT without an index (MPEG-4 with B-frames)" mpeg4 24 -c:v mpeg4 -bf 2 -g 8 -q:v 2 -write_index 0
check ramp_h264_bf_noidx.nut "NUT without an index (H.264 with B-frames)" libx264 24 $X264 -write_index 0
# The largest PTS may be that of the audio: an audio track of the same length adds no frame, a longer one adds
# the frames it covers (the last frame is held, as in Matroska).
check_audio ramp_ffv1_pcm.nut "NUT (FFV1, PCM audio of the same length)" ffv1 1.6667 40 -c:v ffv1 -c:a pcm_s16le
check_audio ramp_ffv1_long_pcm.nut "NUT (FFV1, 2 s PCM audio)" ffv1 2 48 -c:v ffv1 -c:a pcm_s16le
# Files larger than the 4 MB that the end probe reads: the end is found by a seek, through the index or without one.
check_large_nut large_ffv1.nut "Large NUT (FFV1, over 4 MB)" 0 60 -c:v ffv1
check_large_nut large_ffv1_noidx.nut "Large NUT without an index (FFV1, over 4 MB)" 0 60 -c:v ffv1 -write_index 0
check_large_nut large_ffv1_offset.nut "Large NUT (FFV1, over 4 MB, non-zero start time)" 0 60 -c:v ffv1 -output_ts_offset 10
check_large_nut large_ffv1_offset_noidx.nut "Large NUT without an index (FFV1, over 4 MB, non-zero start time)" 0 60 -c:v ffv1 -output_ts_offset 10 -write_index 0
check_large_nut large_ffv1_pcm.nut "Large NUT (FFV1, over 4 MB, 3 s PCM audio)" 3 72 -c:v ffv1
check_large_nut large_ffv1_pcm_noidx.nut "Large NUT without an index (FFV1, over 4 MB, 3 s PCM audio)" 3 72 -c:v ffv1 -write_index 0
# A single GOP longer than the probe: the probe gives up, and the end is the duration plus one frame. Only the
# frame count is checked (--open-only): every seek in a single GOP decodes from its start, which takes 20 to 40 s.
check_large_nut large_h264_gop.nut "Large NUT (lossless H.264, one GOP over 4 MB)" 0 60 \
    -c:v libx264 -qp 0 -g 1000 -x264-params scenecut=0 -pix_fmt yuv420p -- --open-only
check_large_nut large_h264_gop_noidx.nut "Large NUT without an index (lossless H.264, one GOP over 4 MB)" 0 60 \
    -c:v libx264 -qp 0 -g 1000 -x264-params scenecut=0 -pix_fmt yuv420p -write_index 0 -- --open-only
# An elementary stream has no timestamps: the last frame is a reference frame without a PTS.
check ramp_bframes.m2v "MPEG-2 elementary stream (B-frames)" mpeg2video 24 -c:v mpeg2video -bf 2 -g 8 -q:v 2
# An edit list that starts after the first frames (negative timestamps): they are not displayed.
check_frames ramp_preroll1.mp4 "MP4 (H.264, edit list, first frame not displayed: negative timestamp)" libx264 24 39 1 \
    $X264 -output_ts_offset -0.041666666666666667
check_frames ramp_preroll3.mp4 "MP4 (H.264, edit list, first 3 frames not displayed: negative timestamps)" libx264 24 37 3 \
    $X264 -output_ts_offset -0.125
check_frames ramp_preroll2.mov "MOV (H.264, edit list, first 2 frames not displayed: negative timestamps)" libx264 24 38 2 \
    $X264 -output_ts_offset -0.083333333333333333
# AVI has no PTS: the decoder gives the frames the DTS of the packet after which they are output.
check ramp_bframes.avi "AVI (MPEG-4 with B-frames, no PTS)" mpeg4 24 -c:v mpeg4 -bf 2 -g 8 -q:v 2
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
check_forward ramp_forward.mp4 "MP4 (H.264, GOP 50): forward requests in a GOP decode forward, without a seek"
check_forward ramp_forward.mkv "Matroska (H.264, GOP 50): forward requests in a GOP decode forward, without a seek"
check_forward ramp_forward.ts "MPEG-TS (H.264, GOP 50): forward requests within 1 s decode forward, without a seek" -f mpegts
check_forward_ts_jump forward_jump.ts "MPEG-TS (H.264, GOP 50, a 20 s timestamp jump): requests far ahead seek"
check_forward_asf_noindex forward_noindex.asf "ASF (MPEG-4, GOP 25, no index): requests far ahead seek"
check_forward_open_gop forward_open_gop.mp4 "MP4 (HEVC, open GOPs): requests close ahead decode forward, also leading pictures"
# Damaged packets (GOP 8: I0 P3 B1 B2 P6 B4 B5 P7 in decoding order, without scene cuts): the damaged
# frame is a gap, and the frames of its GOP (and the last ones of the previous GOP, which the decoder
# outputs after the next reference frame) show what the decoder conceals.
X264_GOP="-c:v libx264 -g 8 -bf 2 -x264-params b-adapt=0:scenecut=0 -pix_fmt yuv420p"
check_damaged ramp_damaged_p.mkv "MKV (H.264, 24 fps, damaged P-frame)" libx264 N 12 15 7,8,9,10,11,12,13,14,15,16 "" $X264_GOP
check_damaged ramp_damaged_idr.mkv "MKV (H.264, 24 fps, damaged IDR frame)" libx264 N 8 9 7,8,9,10,11,12,13,14,15,16 "" $X264_GOP
check_damaged ramp_damaged_b.mkv "MKV (H.264, 24 fps, damaged B-frame)" libx264 N 18 18 15,16,17,18,19,20,21,22,23,24 "" $X264_GOP
check_damaged ramp_damaged_vfr.mkv "VFR MKV (H.264, 24 fps, gaps, two damaged B-frames)" libx264 \
    "if(lte(N\,12)\,N\,12+2+2*(N-13))" "13 27" "" "" "" $X264_GOP
check_damaged ramp_damaged_gop.mkv "MKV (H.264, 24 fps, damaged first GOP: its frames fail)" libx264 N "0 1 2 3 4 5 6 7" "" "" \
    1,2,3,4,5,6,7,8 $X264_GOP
# Intra-only codecs: after a seek to the damaged frame, the previous frame must be held too, also in a run of
# 4 damaged frames, decoded first in a newly opened file (--fresh), in reverse and in another order.
CHECK_DAMAGED_ARGS="--fresh 22,24,21,23,24,23,22,21" check_damaged ramp_damaged_run_prores.mov "MOV (ProRes, 24 fps, a run of 4 damaged frames)" prores_ks N "20 21 22 23" 21,22,23,24 "" "" -c:v prores_ks -profile:v 2
CHECK_DAMAGED_ARGS="--fresh 22,24,21,23,24,23,22,21" check_damaged ramp_damaged_run_mjpeg.avi "AVI (MJPEG, 24 fps, a run of 4 damaged frames)" mjpeg N "20 21 22 23" 21,22,23,24 "" "" \
    -c:v mjpeg -q:v 2 -pix_fmt yuvj420p
check_damaged ramp_damaged_prores.mov "MOV (ProRes, 24 fps, two damaged frames)" prores_ks N "13 27" 14,28 "" "" -c:v prores_ks
check_damaged ramp_damaged_mjpeg.avi "AVI (MJPEG, 24 fps, two damaged frames)" mjpeg N "13 27" 14,28 "" "" \
    -c:v mjpeg -q:v 2 -pix_fmt yuvj420p
check_ts_cc ramp_cc_error.ts "MPEG-TS (H.264 and MP2, a continuity counter error in frame 18, not a truncated file)"
check_damaged_av1 ramp_damaged_av1.mkv "MKV (AV1 with libdav1d, 24 fps, two damaged frames)" libsvtav1 -c:v libsvtav1 -g 8 -preset 8
# Longer damaged runs in ProRes: frames after a run of 100 damaged packets must decode (in any order), and a
# file damaged from some point on must not be read again for each frame.
check_damaged_long ramp_damaged_run100.mov "MOV (ProRes, 300 frames, packets 100-199 damaged)" "between(n,100,199)" 101 200 201,202,203,204,150 12
check_damaged_long ramp_damaged_from6.mov "MOV (ProRes, 300 frames, damaged from the 7th packet on)" "gte(n,6)" 7 300 7,8,100 12
# A good frame (181) between two damaged runs must not join a damaged range, whatever the order (with frame
# threads, the error of a packet comes with a later one).
# A damaged first keyframe in a long GOP: no frame can be decoded, and the file must be read about twice.
check_damaged_gop damaged_idr_long_gop.mp4 "MP4 (H.264, GOP 300, B-frames, damaged first keyframe)" 250 "if(eq(n,0),1,0)" 1 250 4 \
    -g 300 -bf 2 -movflags +faststart
# A long damaged run (packets 50-150) in a 100-frame GOP with B-frames: after too many damaged packets, the
# decoder still holds the good frames before the run (49 and 52, waiting for their B-frames), which must
# decode, in any order; the frames after the run fail until the next keyframe (frame 201).
check_damaged_gop damaged_run_long_gop.mkv "Matroska (H.264, GOP 100, B-frames, packets 50-150 damaged)" 300 "if(between(n,50,150),1,0)" 53 200 "" \
    -g 100 -bf 2 -x264-params scenecut=0:b-adapt=0
# After a damaged run of more than 64 packets, the frames of the run fail with 1 and with 8 decoder threads (with
# frame threads, the packets after the run were sent when the error of the 65th damaged packet came back).
check_damaged_long ramp_damaged_run65.mov "MOV (ProRes, 300 frames, packets 10-74 and 76 damaged)" "between(n,10,74)+eq(n,76)" 11 75 "" 12
# A damaged run in a long GOP of an H.264 Matroska clip: the frames of the intact GOPs must decode as in the
# clean file, in any order (the decoder restarts at the next keyframe).
check_damaged_mkv_run damaged_g100_50_99.mkv "Matroska (H.264, GOP 100, B-frames, packets 50-99 damaged)" 50 99 "1-48,101-300"
check_damaged_mkv_run damaged_g100_50_113.mkv "Matroska (H.264, GOP 100, B-frames, packets 50-113 damaged)" 50 113 "1-48,201-300"
check_damaged_mkv_run damaged_g100_80_119.mkv "Matroska (H.264, GOP 100, B-frames, packets 80-119 damaged)" 80 119 "1-78,201-300"
check_damaged_mkv_run damaged_g100_120_190.mkv "Matroska (H.264, GOP 100, B-frames, packets 120-190 damaged)" 120 190 "1-118,201-300"
# The same without cues (written to a pipe), and in MP4 (the damage includes the IDR frame of packet 100).
check_damaged_mkv_run damaged_g100_pipe_50_113.mkv "Matroska without cues (H.264, GOP 100, B-frames, packets 50-113 damaged)" 50 113 "1-48,201-300" pipe
check_damaged_mkv_run damaged_g100_pipe_80_119.mkv "Matroska without cues (H.264, GOP 100, B-frames, packets 80-119 damaged)" 80 119 "1-78,201-300" pipe
check_damaged_mkv_run damaged_g100_80_119.mp4 "MP4 (H.264, GOP 100, B-frames, packets 80-119 damaged)" 80 119 "1-78,201-300" mp4
# Only the flags byte of the block headers of the packets 80-119: blocks are lost or merged (lacing), and frame 101 (an
# IDR frame, followed by the next keyframe) must decode.
check_damaged_mkv_run damaged_g100_flags_80_119.mkv "Matroska (H.264, GOP 100, B-frames, block flags of packets 80-119 damaged)" 80 119 "1-80,101-101,201-300" mkv flags
# A GOP of 5 with B-frames and every keyframe from packet 20 damaged: after the damage, the decoder outputs frames whose
# PTS is from before the damage, but whose best_effort_timestamp is the DTS of a later packet; frames 1-18 must decode
# as in the clean file, in any order.
check_damaged_ref damaged_g5_keys.mkv "Matroska (H.264, GOP 5, B-frames, every keyframe from packet 20 damaged)" libx264 400 \
    "if(eq(mod(n,5),0)*gte(n,20),1,0)" "1-18" -g 5 -bf 2 -x264-params scenecut=0
# AVI with MPEG-4 B-frames (the reference frames have no PTS) and packets 60-62 damaged: the frames before the damage
# and after the next keyframe must decode as in the clean file, in any order.
check_damaged_ref damaged_bframes.avi "AVI (MPEG-4, GOP 24, B-frames, packets 60-62 damaged)" mpeg4 200 \
    "if(between(n,60,62),1,0)" "1-59,73-200" -bf 2 -g 24 -q:v 3
# Clean clips decoded with frame threads (issue #159): VP9 with alt-ref frames (superframes), in WebM and Matroska, 8 and
# 10 bits, and without alt-ref frames; AV1.
check_frame_threads vp9_altref.webm "WebM (VP9, GOP 12, alt-ref frames): frame threads" libvpx-vp9 yuv420p -b:v 300k -auto-alt-ref 1
check_frame_threads vp9_altref.mkv "Matroska (VP9, GOP 12, alt-ref frames): frame threads" libvpx-vp9 yuv420p -b:v 300k -auto-alt-ref 1
check_frame_threads vp9_altref10.webm "WebM (VP9 10 bits, GOP 12, alt-ref frames): frame threads" libvpx-vp9 yuv420p10le -b:v 300k -auto-alt-ref 1
check_frame_threads vp9_noaltref.webm "WebM (VP9, GOP 12, no alt-ref frames): frame threads" libvpx-vp9 yuv420p -b:v 300k -auto-alt-ref 0
check_frame_threads av1.mkv "Matroska (AV1, libaom, GOP 12): decoder threads" libaom-av1 yuv420p -b:v 300k -cpu-used 8
# All-intra H.264 (every packet is an IDR frame) with every other packet from packet 20 damaged: decoded as an
# intra-only stream once damage is found (not from the start of the file for each frame), bounded in bytes.
check_damaged_gop damaged_intra_h264.mkv "Matroska (all-intra H.264, every other packet from packet 20 damaged)" 120 \
    "if(eq(mod(n,2),0)*gte(n,20),1,0)" 1 0 6 -preset ultrafast -g 1
check_damaged_long ramp_damaged_good181.mov "MOV (ProRes, 300 frames, packets 100-179 and 181-183 damaged)" \
    "between(n,100,179)+between(n,181,183)" 101 180 "" 12 101,184,181,182
# Intra-only codecs found from the codec descriptor (AV_CODEC_PROP_INTRA_ONLY), with packets 10-12 and 30 damaged: the
# other frames must decode as in the clean file, in any order.
for intra in "v210 mov -pix_fmt yuv422p10le" "rawvideo avi" "huffyuv avi -pix_fmt yuv422p" "jpeg2000 mkv" "jpegls avi -pix_fmt rgb24" \
    "hap mov -pix_fmt rgba"; do
    read -r codec container args <<<"$intra"
    # shellcheck disable=SC2086
    check_damaged_ref "damaged_$codec.$container" "Intra-only $codec in $container (packets 10-12 and 30 damaged)" "$codec" 48 \
        "if(between(n,10,12)+eq(n,30),1,0)" "1-10,14-30,32-48" ${args:-}
done
# Truncated intra-only files: the missing frames fail, sequentially and after a seek.
check_truncated_intra ramp_truncated_prores.mov "MOV (ProRes, faststart, truncated in frame 16)" 16 prores_ks -c:v prores_ks
check_truncated_intra ramp_truncated_mjpeg.mov "MOV (MJPEG, faststart, truncated in frame 16)" 16 mjpeg -c:v mjpeg -q:v 2 -pix_fmt yuvj420p
check_truncated_intra ramp_truncated_v210.mov "MOV (v210, faststart, truncated in frame 16)" 16 v210 -c:v v210 -pix_fmt yuv422p10le
check_truncated_intra ramp_truncated_h264.mp4 "MP4 (H.264 with B-frames, faststart, truncated in the 16th packet)" 14 libx264 \
    -c:v libx264 -g 8 -bf 2 -x264-params b-adapt=0:scenecut=0 -pix_fmt yuv420p

# Finding the end of the movie must not read the whole file: a 60 s Matroska with a start time (with
# cues), and FLV files truncated by 7, 377 and 12345 bytes (their last tag is not valid: the duration is
# used, and the tags are not walked back to the start of the file).
if have_encoder libx264; then
    if ffmpeg -y -loglevel error -f lavfi -i testsrc2=s=320x240:r=24 -t 60 -c:v libx264 -preset ultrafast -g 48 -output_ts_offset 10 "$tmp/long_offset.mkv" &&
        ffmpeg -y -loglevel error -f lavfi -i testsrc2=s=320x240:r=24 -t 60 -c:v libx264 -preset ultrafast -g 48 -bf 2 "$tmp/long.flv"; then
        check_open long_offset.mkv "Matroska (H.264, 24 fps, 60 s, start time 10 s): bytes read at open" 1440 2000000
        check_open long.flv "FLV (H.264 with B-frames, 24 fps, 60 s): bytes read at open, index after the open" 1440 2000000 --max-indexed-frame 720
        size=$(wc -c < "$tmp/long.flv")
        for cut in 7 377 12345; do
            head -c $((size - cut)) "$tmp/long.flv" > "$tmp/long_cut$cut.flv"
            # (the last tag is not valid: the duration is used, counted from the start of the video, as before,
            # which includes the B-frame delay)
            check_open "long_cut$cut.flv" "FLV (H.264 with B-frames, 24 fps, 60 s, truncated by $cut bytes): frame count and bytes read at open" 1442 2000000
        done
    else
        echo "FAIL: could not generate the 60 s clips"
        rc=1
    fi
fi
# A high bit rate FLV (about 400 kB per frame, more than in the first windows read at the end) without the
# duration metadata, with and without a start time: the end must still be read from the last packets.
if have_encoder libx264 && have_encoder aac; then
    for offset in 0 10; do
        if ffmpeg -y -loglevel error -f lavfi -i testsrc2=s=640x360:r=25:d=1.2 -f lavfi -i sine=d=1.2 -vf noise=alls=40:allf=t \
            -c:v libx264 -preset ultrafast -bf 3 -qp 0 -g 250 -pix_fmt yuv420p -c:a aac -flvflags no_duration_filesize \
            -output_ts_offset "$offset" "$tmp/high_bitrate_$offset.flv"; then
            check_open "high_bitrate_$offset.flv" "FLV (H.264 with B-frames, 400 kB per frame, no duration metadata, start time $offset s)" 30 40000000
        else
            echo "FAIL: could not generate high_bitrate_$offset.flv"
            rc=1
        fi
    done
fi
# Matroska with a single keyframe, and Matroska without cues (written to a pipe), 160x120 noise (about 9 MB):
# opening must not read the file (the Matroska demuxer parses the clusters to seek without cues,
# libavformat/matroskadec.c:4518-4527 in FFmpeg 9.0.2), and decoding frame 3 after the open reads it at most once
# and a half.
if have_encoder libx264; then
    for kind in onekey pipe; do
        if [ "$kind" = onekey ]; then
            ffmpeg -y -loglevel error -f lavfi -i "nullsrc=s=160x120:r=25,geq=random(1)*255:128:128" -t 20 -c:v libx264 -preset ultrafast \
                -crf 10 -g 100000 -x264-params keyint=infinite:scenecut=0 -pix_fmt yuv420p "$tmp/noise_$kind.mkv"
        else
            ffmpeg -y -loglevel error -f lavfi -i "nullsrc=s=160x120:r=25,geq=random(1)*255:128:128" -t 20 -c:v libx264 -preset ultrafast \
                -crf 10 -g 250 -pix_fmt yuv420p -f matroska pipe:1 > "$tmp/noise_$kind.mkv"
        fi
        size=$(wc -c < "$tmp/noise_$kind.mkv")
        check_open "noise_$kind.mkv" "Matroska ($kind, 20 s of noise, $size bytes): bytes read at open, and decoding frame 3" 500 1000000 \
            --seek-bytes 3,$((size * 3 / 2))
        rm -f "$tmp/noise_$kind.mkv"
    done
fi
# MPEG-TS/PS, whose end is read from the last packets: two programs in a TS (the second starting at 100 s:
# only the program of the video stream counts), an MPEG-PS whose last reference frame has no PTS (it is
# displayed after the B-frames that follow it), VOBs with B-frames, with and without audio (the last
# reference frame has no PTS, and no B-frame follows it). The expected count is the number of video packets.
check_packets() {
    local name="$1" desc="$2" threads
    shift 2
    echo "== $desc =="
    if ! command -v ffprobe >/dev/null 2>&1; then
        echo "SKIPPED: no ffprobe CLI"
        return
    fi
    local packets
    # (sed -n 1p, not head -n 1: with pipefail, ffprobe killed by SIGPIPE after head exits would fail the script)
    packets=$(ffprobe -v error -count_packets -select_streams v:0 -show_entries stream=nb_read_packets -of csv=p=0 "$tmp/$name" | sed -n 1p | tr -d ',')
    for threads in 1 8; do
        # shellcheck disable=SC2086
        FFMPEG_READ_TEST_THREADS=$threads "$bin" "$tmp/$name" --frames "$packets" --open-only "$@" ${FFMPEG_READ_TEST_ARGS:-} || rc=1
    done
}
# An MPEG-PS cut at several offsets (in the middle of a GOP, or on an I-frame followed by the leading B-frames
# of an open GOP): the frames are counted from the start time of the stream (its first PTS) to the last frame
# displayed. The leading B-frames of an open GOP have a PTS before it, and are not counted; after a cut in the
# middle of a GOP, the frames after the start time that cannot be decoded (their reference frames are cut)
# are counted, and fail. The expected count is from the frames decoded by ffprobe (25 fps): from the start
# time to the last decoded frame with a timestamp, plus the frames decoded after it without a timestamp (the
# last reference frame). The count must not depend on the decoder threads.
check_ps_cuts() {
    local name="$1" desc="$2" offset threads start expected
    shift 2
    if ! command -v ffprobe >/dev/null 2>&1; then
        echo "== $desc =="
        echo "SKIPPED: no ffprobe CLI"
        return
    fi
    for offset in "$@"; do
        echo "== $desc (cut at byte $offset) =="
        tail -c +"$offset" "$tmp/$name" > "$tmp/cut_$offset.mpg"
        start=$(ffprobe -v error -select_streams v:0 -show_entries stream=start_time -of csv=p=0 "$tmp/cut_$offset.mpg" | sed -n 1p | tr -d ',\r')
        expected=$(ffprobe -v error -select_streams v:0 -show_entries frame=best_effort_timestamp_time -of csv=p=0 "$tmp/cut_$offset.mpg" 2>/dev/null | tr -d ',\r' |
            awk -v s="$start" 'BEGIN { m = -1; na = 0 } $1 == "N/A" { if (m >= 0) na++ } $1 != "N/A" { if ($1 + 0 > m) { m = $1 + 0; na = 0 } } END { printf "%d\n", int((m - s) * 25 + 1.5) + na }')
        for threads in 1 8; do
            # shellcheck disable=SC2086
            FFMPEG_READ_TEST_THREADS=$threads "$bin" "$tmp/cut_$offset.mpg" --frames "$expected" --open-only ${FFMPEG_READ_TEST_ARGS:-} || rc=1
        done
    done
}
if have_encoder mpeg2video && have_encoder mp2 && have_encoder ac3; then
    if ffmpeg -y -loglevel error -f lavfi -i testsrc=d=4 -itsoffset 100 -f lavfi -i testsrc2=d=4 -map 0:v -map 1:v \
        -c:v mpeg2video -program st=0 -program st=1 "$tmp/two_programs.ts" &&
        ffmpeg -y -loglevel error -f lavfi -i "$(ramp 24)" -frames:v "$NFRAMES" -c:v mpeg2video -bf 2 -g 8 -q:v 2 -f mpeg "$tmp/ramp_ps.mpg" &&
        ffmpeg -y -loglevel error -f lavfi -i testsrc2=s=320x240:r=25 -f lavfi -i sine -t 10 -c:v mpeg2video -bf 2 -g 12 -c:a mp2 -f vob "$tmp/b_frames.vob" &&
        ffmpeg -y -loglevel error -f lavfi -i testsrc2=s=320x240:r=25 -t 8 -c:v mpeg2video -bf 2 -g 12 -f vob "$tmp/vob_no_audio.vob" &&
        ffmpeg -y -loglevel error -f lavfi -i testsrc2=s=320x240:r=25 -f lavfi -i sine -t 8 -c:v mpeg2video -bf 2 -g 12 -c:a ac3 -f vob "$tmp/vob_ac3.vob" &&
        ffmpeg -y -loglevel error -f lavfi -i testsrc2=s=640x360:r=25 -f lavfi -i sine -t 10 -c:v mpeg2video -b:v 4M -bf 2 -g 15 -c:a mp2 -f mpeg "$tmp/full_ps.mpg"; then
        check_packets two_programs.ts "MPEG-TS (two programs, the second one starting at 100 s)"
        check_packets ramp_ps.mpg "MPEG-PS (MPEG-2 with B-frames, the last reference frame has no PTS)"
        check_packets b_frames.vob "VOB (MPEG-2 with B-frames, MP2 audio)"
        check_packets vob_no_audio.vob "VOB (MPEG-2 with B-frames, no audio, the last reference frame has no PTS)"
        check_packets vob_ac3.vob "VOB (MPEG-2 with B-frames, AC3 audio, the last reference frame has no PTS)"
        check_ps_cuts full_ps.mpg "MPEG-PS cut" 300000 777777 1000001 1234568 1269589 1500000 2222223 2600000 3000000 4000000
    else
        echo "FAIL: could not generate the MPEG-TS/PS clips"
        rc=1
    fi
fi
# An MPEG-TS whose end probe fails: 1 s of video and 8 s of 8-channel 32-bit SMPTE 302M audio (11 MB), so the
# last 4 MB have no video packet. The end is FFmpeg's estimate, and the last frame is held until the end of the
# audio (192 frames).
echo "== MPEG-TS (MPEG-2 with B-frames, 8 s of SMPTE 302M audio after 1 s of video) =="
if have_encoder mpeg2video && have_encoder s302m; then
    if ffmpeg -y -loglevel error -f lavfi -i color=c=gray:s=128x128:r=24:d=1 -f lavfi -i anoisesrc=r=48000:d=8:a=0.5 \
        -map 0:v -map 1:a -c:v mpeg2video -bf 2 -c:a s302m -strict -2 -ac 8 -sample_fmt s32 "$tmp/s302m_long.ts"; then
        # shellcheck disable=SC2086
        "$bin" "$tmp/s302m_long.ts" --frames 192 --open-only ${FFMPEG_READ_TEST_ARGS:-} || rc=1
    else
        echo "FAIL: could not generate s302m_long.ts"
        rc=1
    fi
else
    echo "SKIPPED: no mpeg2video or s302m encoder in the ffmpeg CLI"
fi
# A stream copy from a non-keyframe: fewer samples than frame times.
check_ss_copy ramp_ss_copy.mov "MOV (stream copy from 0.6 s, edit list)" 43 0.6
check_ss_copy ramp_ss_copy.mp4 "MP4 (stream copy from 1.3 s, no edit list)" 53 1.3 -use_editlist 0
# VFR MP4 with a one-frame gap, in the middle and before the last frame: the last frame must be counted.
check_vfr ramp_one_gap.mp4 "VFR MP4 (H.264, 24 fps, a one-frame gap)" 41 20 2 1 0 42
check_vfr ramp_end_one_gap.mp4 "VFR MP4 (H.264, 24 fps, a one-frame gap before the last frame)" 41 39 2 1 0 42

# Y'CbCr matrix and range of the conversion to RGB: clips of a constant Y'CbCr color (written as raw yuv444p
# bytes, encoded losslessly, and checked to decode to the same bytes), tagged with each matrix and range, or
# untagged. The expected RGB is computed here from the definition of the matrix (ITU-T H.273: Kr and Kb) and of the
# range (video: Y' = (Y - 16) / 219, Pb = (Cb - 128) / 224, full: Y' = Y / 255, Pb = (Cb - 128) / 255, scaled by
# 2^(bits - 8)), not with FFmpeg. Auto uses the tags of the file (AVFrame.colorspace and color_range), else Rec. 601
# below 720 lines, Rec. 709 from 720 lines, and video range; --matrix and --range override them.
expected_rgb() {
    # expected_rgb Y CB CR BITS MATRIX RANGE: R,G,B on a 0-255 scale; MATRIX is 601, 709, 2020, fcc or 240m;
    # RANGE is tv or pc
    awk -v y="$1" -v cb="$2" -v cr="$3" -v bits="$4" -v m="$5" -v r="$6" 'BEGIN {
        if (m == "601") { kr = 0.299; kb = 0.114 } else if (m == "709") { kr = 0.2126; kb = 0.0722 }
        else if (m == "2020") { kr = 0.2627; kb = 0.0593 } else if (m == "fcc") { kr = 0.30; kb = 0.11 }
        else if (m == "240m") { kr = 0.212; kb = 0.087 } else { exit 1 }
        s = 2 ^ (bits - 8)
        if (r == "tv") { yp = (y - 16 * s) / (219 * s); pb = (cb - 128 * s) / (224 * s); pr = (cr - 128 * s) / (224 * s) }
        else { yp = y / (255 * s); pb = (cb - 128 * s) / (255 * s); pr = (cr - 128 * s) / (255 * s) }
        R = yp + 2 * (1 - kr) * pr; B = yp + 2 * (1 - kb) * pb; G = (yp - kr * R - kb * B) / (1 - kr - kb)
        printf "%.2f,%.2f,%.2f\n", 255 * R, 255 * G, 255 * B
    }'
}
constant_yuv() {
    # constant_yuv W H N BITS Y CB CR: N frames of raw yuv444p (or yuv444p10le) of a constant color on stdout
    perl -e 'my ($w, $h, $n, $bits, @v) = @ARGV; my $np = $w * $h;
        my $f = join("", map { $bits > 8 ? pack("v", $_) x $np : chr($_) x $np } @v); print $f for 1 .. $n' "$@"
}
check_color() {
    # check_color NAME DESC CODEC W H BITS Y CB CR MATRIX RANGE [TAGS -- READER_ARGS]
    # TAGS are the ffmpeg -colorspace/-color_range/-color_primaries/-color_trc options of the clip (also given to the
    # rawvideo input, so that ffmpeg does not convert the samples); MATRIX and RANGE are the expected conversion.
    local name="$1" desc="$2" codec="$3" w="$4" h="$5" bits="$6" y="$7" cb="$8" cr="$9" matrix="${10}" range="${11}"
    shift 11
    echo "== $desc =="
    local tags=() readerargs=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do
        tags+=("$1")
        shift
    done
    if [ $# -gt 0 ]; then
        shift
        readerargs=("$@")
    fi
    local pixfmt=yuv444p encargs=()
    if [ "$bits" -gt 8 ]; then
        pixfmt=yuv444p10le
    fi
    case "$codec" in
    libx264) encargs=(-c:v libx264 -qp 0 -preset ultrafast) ;;
    ffv1) encargs=(-c:v ffv1) ;;
    *) encargs=(-c:v "$codec") ;;
    esac
    if ! have_encoder "$codec"; then
        echo "SKIPPED: no $codec encoder in the ffmpeg CLI"
        return
    fi
    constant_yuv "$w" "$h" 3 "$bits" "$y" "$cb" "$cr" >"$tmp/$name.yuv"
    if ! ffmpeg -y -loglevel error -f rawvideo -pix_fmt "$pixfmt" -s "${w}x$h" -r 24 ${tags[@]+"${tags[@]}"} -i "$tmp/$name.yuv" \
        "${encargs[@]}" -pix_fmt "$pixfmt" ${tags[@]+"${tags[@]}"} "$tmp/$name"; then
        echo "FAIL: could not generate $name"
        rc=1
        return
    fi
    # the clip must hold the constant color exactly (lossless, and not converted by ffmpeg). The samples are written
    # in the decoder's own pixel format, without -pix_fmt: for a full-range clip, FFmpeg's H.264 decoder gives
    # yuvj444p (libavcodec/h264_slice.c:878-884 at n9.0.2), which has the layout of yuv444p, and a -pix_fmt would
    # insert a scale filter.
    if ! ffmpeg -loglevel error -i "$tmp/$name" -f rawvideo - | cmp -s - "$tmp/$name.yuv"; then
        echo "FAIL: $name does not decode to the Y'CbCr samples it was encoded from"
        rc=1
        return
    fi
    ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt,color_space,color_range,color_primaries,color_transfer -of compact=p=0 "$tmp/$name" || true
    local rgb
    rgb="$(expected_rgb "$y" "$cb" "$cr" "$bits" "$matrix" "$range")"
    # shellcheck disable=SC2086
    "$bin" "$tmp/$name" --open-only --rgb "$rgb" ${readerargs[@]+"${readerargs[@]}"} ${FFMPEG_READ_TEST_ARGS:-} || rc=1
    rm -f "$tmp/$name.yuv"
}
# A saturated orange: the matrices give R values 6 to 14 levels apart (8 bits), and the ranges differ everywhere.
CY=120 CB=80 CR=200
check_color color_untagged_sd.mp4 "Y'CbCr matrix: untagged SD H.264 (Auto: Rec. 601, video range)" libx264 64 48 8 $CY $CB $CR 601 tv
check_color color_untagged_hd.mp4 "Y'CbCr matrix: untagged 720p H.264 (Auto: Rec. 709, video range)" libx264 1280 720 8 $CY $CB $CR 709 tv
check_color color_bt709_tv.mp4 "Y'CbCr matrix: SD H.264 tagged bt709, video range" libx264 64 48 8 $CY $CB $CR 709 tv \
    -colorspace bt709 -color_range tv -color_primaries bt709 -color_trc bt709
check_color color_bt709_pc.mp4 "Y'CbCr matrix: SD H.264 tagged bt709, full range" libx264 64 48 8 $CY $CB $CR 709 pc \
    -colorspace bt709 -color_range pc -color_primaries bt709 -color_trc bt709
check_color color_bt470bg_tv.mp4 "Y'CbCr matrix: 720p H.264 tagged bt470bg, video range" libx264 1280 720 8 $CY $CB $CR 601 tv \
    -colorspace bt470bg -color_range tv -color_primaries bt470bg -color_trc gamma28
check_color color_smpte170m_pc.mp4 "Y'CbCr matrix: 720p H.264 tagged smpte170m, full range" libx264 1280 720 8 $CY $CB $CR 601 pc \
    -colorspace smpte170m -color_range pc -color_primaries smpte170m -color_trc smpte170m
check_color color_bt2020nc_tv.mp4 "Y'CbCr matrix: SD H.264 tagged bt2020nc, video range" libx264 64 48 8 $CY $CB $CR 2020 tv \
    -colorspace bt2020nc -color_range tv -color_primaries bt2020 -color_trc bt2020-10
check_color color_bt2020nc_pc.mp4 "Y'CbCr matrix: SD H.264 tagged bt2020nc, full range" libx264 64 48 8 $CY $CB $CR 2020 pc \
    -colorspace bt2020nc -color_range pc -color_primaries bt2020 -color_trc bt2020-10
check_color color_fcc_tv.mp4 "Y'CbCr matrix: SD H.264 tagged fcc, video range" libx264 64 48 8 $CY $CB $CR fcc tv \
    -colorspace fcc -color_range tv
check_color color_smpte240m_tv.mp4 "Y'CbCr matrix: SD H.264 tagged smpte240m, video range" libx264 64 48 8 $CY $CB $CR 240m tv \
    -colorspace smpte240m -color_range tv
# The tags of the container only (FFV1 has none in its bitstream): Matroska Colour element, 10 bits.
check_color color_bt2020nc_10bit.mkv "Y'CbCr matrix: SD FFV1 10-bit in Matroska tagged bt2020nc, video range" ffv1 64 48 10 \
    $((CY * 4)) $((CB * 4)) $((CR * 4)) 2020 tv -colorspace bt2020nc -color_range tv -color_primaries bt2020 -color_trc smpte2084
check_color color_bt709_pc_10bit.mkv "Y'CbCr matrix: SD FFV1 10-bit in Matroska tagged bt709, full range" ffv1 64 48 10 \
    $((CY * 4)) $((CB * 4)) $((CR * 4)) 709 pc -colorspace bt709 -color_range pc
# The params override the tags.
check_color color_override_untagged.mp4 "Y'CbCr matrix: untagged SD H.264 read as Rec. 2020, full range (params)" libx264 64 48 8 $CY $CB $CR 2020 pc \
    -- --matrix 3 --range 2
check_color color_override_bt2020.mp4 "Y'CbCr matrix: SD H.264 tagged bt2020nc, full range, read as Rec. 709, video range (params)" libx264 64 48 8 $CY $CB $CR 709 tv \
    -colorspace bt2020nc -color_range pc -- --matrix 2 --range 1
check_color color_override_601.mp4 "Y'CbCr matrix: 720p H.264 tagged bt709, read as Rec. 601 (param), Auto range" libx264 1280 720 8 $CY $CB $CR 601 tv \
    -colorspace bt709 -color_range tv -- --matrix 1

# Bit depth (issue #213): 3 frames of a horizontal gray ramp, one code value per column (876x64, BITS-bit luma
# (64 + X) * 2^(BITS - 10) in video range, neutral chroma; 8 bits: 16 + X / 4), in PIXFMT. The reader must report BITS
# bits: the largest component depth of the decoded pixel format (the vp9, hevc and libdav1d decoders leave
# bits_per_raw_sample at 0, and av_get_bits_per_pixel() / 3 gave 15 / 3 = 5 bits for yuv420p10, which was decoded to
# 8 bits), or for a Bayer format, whose components share one sample, its bits per pixel. Above 8 bits, the ramp must
# decode to at least 512 distinct values (8 bits give at most 256, here 219) and without the steps of 8 bits: adjacent
# columns must differ by at most 0.9 on a 0-255 scale (8-bit output steps by 1 or 2 levels every 4 columns; 10 bits by
# 255 / 876 = 0.29 every column, plus the noise of the lossy codecs, up to 0.6). lavfi cannot make Bayer frames: a
# bayer_rggb16le PIXFMT reads a 16-bit gray ramp (raw gray16le, 64 * (64 + X)) as Bayer samples. A stream that the
# libavcodec of the harness cannot decode (e.g. AV1 without libdav1d) is skipped (--skip-without-decoder).
# check_bitdepth NAME DESCRIPTION BITS PIXFMT ENCODER [FFMPEG_OUTPUT_ARGS...]
check_bitdepth() {
    local name="$1" desc="$2" bits="$3" pixfmt="$4" encoder="$5" lum chroma got status
    shift 5
    echo "== $desc =="
    case "$bits" in
    8) lum="16+trunc(X/4)" ;;
    10) lum="64+X" ;;
    12) lum="4*(64+X)" ;;
    16) lum="64*(64+X)" ;;
    *) echo "FAIL: check_bitdepth: unsupported bit depth $bits"; rc=1; return ;;
    esac
    chroma=$((1 << (bits - 1)))
    if ! have_encoder "$encoder"; then
        echo "SKIPPED: no $encoder encoder in the ffmpeg CLI"
        return
    fi
    if ! command -v ffprobe >/dev/null 2>&1; then
        echo "SKIPPED: no ffprobe CLI"
        return
    fi
    local input=(-f lavfi -i "nullsrc=s=876x64:r=24,format=$pixfmt,geq=lum='$lum':cb=$chroma:cr=$chroma")
    if [[ "$pixfmt" == bayer_* ]]; then
        if ! ffmpeg -y -loglevel error -f lavfi -i "nullsrc=s=876x64:r=24,format=gray16le,geq=lum='$lum'" -frames:v 3 \
            -f rawvideo "$tmp/$name.raw" 2>"$tmp/$name.log"; then
            echo "SKIPPED: the ffmpeg CLI could not make the gray16le ramp: $(head -1 "$tmp/$name.log")"
            return
        fi
        input=(-f rawvideo -pix_fmt "$pixfmt" -s 876x64 -r 24 -i "$tmp/$name.raw")
    fi
    if ! ffmpeg -y -loglevel error "${input[@]}" -frames:v 3 -c:v "$encoder" "$@" -pix_fmt "$pixfmt" "$tmp/$name" \
        2>"$tmp/$name.log"; then
        echo "SKIPPED: the ffmpeg CLI could not encode $pixfmt with $encoder: $(head -1 "$tmp/$name.log")"
        return
    fi
    got="$(ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt -of csv=p=0 "$tmp/$name")"
    if [ "$got" != "$pixfmt" ]; then
        echo "SKIPPED: $encoder wrote $got, not $pixfmt"
        return
    fi
    local args=(--open-only --skip-without-decoder --bit-depth "$bits")
    if [ "$bits" -gt 8 ]; then
        args+=(--fine-ramp 0.9,512)
    fi
    status=0
    # shellcheck disable=SC2086
    "$bin" "$tmp/$name" "${args[@]}" ${FFMPEG_READ_TEST_ARGS:-} || status=$?
    if [ "$status" -ne 0 ] && [ "$status" -ne 77 ]; then
        rc=1
    fi
    rm -f "$tmp/$name.raw"
}
check_bitdepth bitdepth_vp9_8.webm "Bit depth: VP9 profile 0, 8 bits, lossless (control)" 8 yuv420p libvpx-vp9 -lossless 1
check_bitdepth bitdepth_vp9_10.webm "Bit depth: VP9 profile 2, 10 bits, lossless" 10 yuv420p10le libvpx-vp9 -lossless 1
check_bitdepth bitdepth_vp9_12.mkv "Bit depth: VP9 profile 2, 12 bits, lossless" 12 yuv420p12le libvpx-vp9 -lossless 1
check_bitdepth bitdepth_vp9_422_10.webm "Bit depth: VP9 profile 3, 4:2:2 10 bits, lossless" 10 yuv422p10le libvpx-vp9 -lossless 1
check_bitdepth bitdepth_av1_10.mkv "Bit depth: AV1 10 bits (SVT-AV1, decoded by libdav1d)" 10 yuv420p10le libsvtav1 -crf 10 -preset 8
check_bitdepth bitdepth_hevc_10.mp4 "Bit depth: HEVC Main 10, lossless (libx265)" 10 yuv420p10le libx265 -x265-params lossless=1:log-level=error
check_bitdepth bitdepth_hevc_vt_10.mp4 "Bit depth: HEVC Main 10 (VideoToolbox)" 10 yuv420p10le hevc_videotoolbox -profile:v main10 -q:v 80
check_bitdepth bitdepth_h264_10.mkv "Bit depth: H.264 High 10 (libx264, sets bits_per_raw_sample)" 10 yuv420p10le libx264 -profile:v high10 -qp 1 -preset ultrafast
# Bayer (FFV1 sets bits_per_raw_sample to 16, and the components of bayer_rggb16 have 4, 8 and 4 bits)
check_bitdepth bitdepth_ffv1_bayer16.mkv "Bit depth: FFV1 bayer_rggb16le" 16 bayer_rggb16le ffv1 -level 4 -strict experimental

if [ "$rc" -eq 0 ]; then
    echo "ALL READ-PATH TESTS PASSED"
else
    echo "READ-PATH TESTS FAILED"
fi
exit "$rc"
