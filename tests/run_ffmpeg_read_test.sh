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
#   * NUT (MPEG-4 with B-frames), which has no start time, and an MPEG-2 elementary stream
#     with B-frames, which has no timestamps.
#   * MP4 with an edit list that does not display the first 1 or 3 frames (negative
#     timestamps): they are counted in the stream's number of frames, but not displayed.
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
check_mkv_length ramp_length.mkv "Matroska (H.264, 24 fps, GOP 8, start time 0.5 s, duration written as a length, as mkvmerge does)" -g 8
check_mkv_length ramp_length_gop.mkv "Matroska (H.264, 24 fps, default GOP, start time 0.5 s, duration written as a length)"
check_flv_overhang ramp_overhang.flv "FLV (H.264 with B-frames, 24 fps, no duration metadata, AAC audio ending 40 ms after the video)"
# MP4/MOV with B-frames and no edit list: the start time is the B-frame delay.
check ramp_frag.mp4 "Fragmented MP4 (H.264 with B-frames, no edit list)" libx264 24 $X264 -movflags frag_keyframe+empty_moov
check ramp_noeditlist.mov "MOV (H.264 with B-frames, no edit list)" libx264 24 $X264 -use_editlist 0
check ramp_hevc_frag.mp4 "Fragmented MP4 (HEVC with B-frames, no edit list)" libx265 24 \
    -c:v libx265 -x265-params log-level=error:bframes=3:keyint=8 -tag:v hvc1 -pix_fmt yuv420p -movflags frag_keyframe+empty_moov
# NUT has no start time, and its duration is counted from the first PTS.
check ramp_mpeg4_bf.nut "NUT (MPEG-4 with B-frames)" mpeg4 24 -c:v mpeg4 -bf 2 -g 8 -q:v 2
# An elementary stream has no timestamps: the last frame is a reference frame without a PTS.
check ramp_bframes.m2v "MPEG-2 elementary stream (B-frames)" mpeg2video 24 -c:v mpeg2video -bf 2 -g 8 -q:v 2
# An edit list that starts after the first frames (negative timestamps): they are not displayed.
check_frames ramp_preroll1.mp4 "MP4 (H.264, edit list, first frame not displayed: negative timestamp)" libx264 24 39 1 \
    $X264 -output_ts_offset -0.041666666666666667
check_frames ramp_preroll3.mp4 "MP4 (H.264, edit list, first 3 frames not displayed: negative timestamps)" libx264 24 37 3 \
    $X264 -output_ts_offset -0.125
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
# A stream copy from a non-keyframe: fewer samples than frame times.
check_ss_copy ramp_ss_copy.mov "MOV (stream copy from 0.6 s, edit list)" 43 0.6
check_ss_copy ramp_ss_copy.mp4 "MP4 (stream copy from 1.3 s, no edit list)" 53 1.3 -use_editlist 0
# VFR MP4 with a one-frame gap, in the middle and before the last frame: the last frame must be counted.
check_vfr ramp_one_gap.mp4 "VFR MP4 (H.264, 24 fps, a one-frame gap)" 41 20 2 1 0 42
check_vfr ramp_end_one_gap.mp4 "VFR MP4 (H.264, 24 fps, a one-frame gap before the last frame)" 41 39 2 1 0 42

if [ "$rc" -eq 0 ]; then
    echo "ALL READ-PATH TESTS PASSED"
else
    echo "READ-PATH TESTS FAILED"
fi
exit "$rc"
