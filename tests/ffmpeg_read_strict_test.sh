#!/usr/bin/env bash
# ffmpeg_read_strict_test.sh: the strict mode of run_ffmpeg_read_test.sh (#198). With ffmpeg and ffprobe stubs that
# have no encoder but libsvtav1, and make only the clip of the AV1 bit-depth section (bitdepth_av1_10.mkv), every
# section of run_ffmpeg_read_test.sh skips: that one through the harness, which exits with status 77 ("no decoder in
# the linked libavcodec"), the others in the script.
#   - by default, the run passes and its last line counts the skipped sections, at least MIN_SKIPS (all of today's
#     sections: a block that skips without a SKIPPED line lowers the count);
#   - with FFMPEG_READ_TEST_STRICT=1, it fails, with one "FAIL: unexpected skip" line per skipped section;
#   - with FFMPEG_READ_TEST_STRICT=1 and FFMPEG_READ_TEST_EXPECTED_SKIPS matching every reason, it passes, and a
#     pattern that matches only one reason (the harness's) fails the others;
#   - an invalid FFMPEG_READ_TEST_EXPECTED_SKIPS is an error (exit status 2), reported once.
# FFMPEG_READ_TEST_BIN is a stub: it exits with status 77 on the AV1 clip, and passes the checks that need no clip
# (FFmpegFile::isImageFile()).
#
# Usage: tests/ffmpeg_read_strict_test.sh. Exit status: 0 on success, 1 on failure.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# the sections of run_ffmpeg_read_test.sh today (the VideoToolbox one only runs on macOS); raise it with new sections
MIN_SKIPS=215
if [ "$(uname -s)" = Darwin ]; then
    MIN_SKIPS=216
fi

mkdir "$work/bin"
cat > "$work/bin/ffmpeg" <<'EOF'
#!/bin/sh
for a; do last=$a; done
case " $* " in
*" -h encoder=libsvtav1 "*) echo "Encoder libsvtav1 [SVT-AV1]:"; exit 0 ;;
esac
case "$last" in */bitdepth_av1_10.mkv) : > "$last"; exit 0 ;; esac
exit 1
EOF
cat > "$work/bin/ffprobe" <<'EOF'
#!/bin/sh
for a; do last=$a; done
case "$last" in */bitdepth_av1_10.mkv) echo yuv420p10le; exit 0 ;; esac
exit 1
EOF
cat > "$work/bin/harness" <<'EOF'
#!/bin/sh
case "$1" in */bitdepth_av1_10.mkv) echo "SKIPPED: no decoder in the linked libavcodec: stub"; exit 77 ;; esac
exit 0
EOF
chmod +x "$work/bin/ffmpeg" "$work/bin/ffprobe" "$work/bin/harness"

rc=0
# run NAME EXPECTED_STATUS [VAR=VALUE...]: runs run_ffmpeg_read_test.sh with the stubs and VAR=VALUE; its output is in
# $work/NAME.log
run() {
    local name="$1" expected="$2" status=0
    shift 2
    env -u FFMPEG_READ_TEST_STRICT -u FFMPEG_READ_TEST_EXPECTED_SKIPS -u FFMPEG_READ_TEST_KEEP \
        PATH="$work/bin:$PATH" FFMPEG_READ_TEST_BIN="$work/bin/harness" "$@" bash "$here/run_ffmpeg_read_test.sh" \
        > "$work/$name.log" 2>&1 || status=$?
    if [ "$status" -ne "$expected" ]; then
        echo "FAIL: $name: exit status $status, expected $expected; its last lines:"
        tail -5 "$work/$name.log"
        rc=1
        return 1
    fi
}
count() { grep -c -- "$1" "$work/$2.log" || true; }
harness_reason='no decoder in the linked libavcodec: bitdepth_av1_10.mkv'

if run default 0; then
    skipped=$(count '^SKIPPED: ' default)
    if [ "$skipped" -lt "$MIN_SKIPS" ] ||
        ! tail -1 "$work/default.log" | grep -q "^ALL READ-PATH TESTS PASSED ($skipped sections not run)$"; then
        echo "FAIL: default: $skipped SKIPPED lines (at least $MIN_SKIPS expected), last line: $(tail -1 "$work/default.log")"
        rc=1
    elif [ "$(count '^SKIPPED: no decoder in the linked libavcodec: stub$' default)" -ne 1 ]; then
        echo "FAIL: default: the harness's skip (exit status 77) is missing"
        rc=1
    elif [ "$(count '^FAIL' default)" -ne 0 ]; then
        echo "FAIL: default: FAIL lines without FFMPEG_READ_TEST_STRICT"
        rc=1
    else
        echo "PASS: default: the run passes, $skipped sections not run"
    fi
fi
if run strict 1 FFMPEG_READ_TEST_STRICT=1; then
    skipped=$(count '^SKIPPED: ' strict)
    unexpected=$(count '^FAIL: unexpected skip (FFMPEG_READ_TEST_STRICT=1): ' strict)
    harness=$(count "^FAIL: unexpected skip (FFMPEG_READ_TEST_STRICT=1): $harness_reason\$" strict)
    if [ "$unexpected" -ne "$skipped" ] || [ "$skipped" -lt "$MIN_SKIPS" ] ||
        [ "$(count '^FAIL' strict)" -ne "$unexpected" ] || [ "$harness" -ne 1 ]; then
        echo "FAIL: strict: $skipped SKIPPED lines, $unexpected unexpected skips ($harness of the harness)," \
            "$(count '^FAIL' strict) FAIL lines"
        rc=1
    else
        echo "PASS: strict: the run fails, $unexpected unexpected skips (also the harness's)"
    fi
fi
# every reason of the stubs: a missing encoder, ffprobe or decoder, or a clip that could not be made
if run expected 0 FFMPEG_READ_TEST_STRICT=1 \
    'FFMPEG_READ_TEST_EXPECTED_SKIPS=^(no |the ffmpeg CLI could not |[^ ]+ wrote )'; then
    if [ "$(count '^FAIL' expected)" -ne 0 ]; then
        echo "FAIL: expected: FAIL lines although every skip is expected"
        rc=1
    else
        echo "PASS: expected: the run passes, all skips are expected"
    fi
fi
# a pattern that matches only the harness's reason, as its SKIPPED line gives it: the others still fail
if run partial 1 FFMPEG_READ_TEST_STRICT=1 'FFMPEG_READ_TEST_EXPECTED_SKIPS=^no decoder in the linked libavcodec: '; then
    skipped=$(count '^SKIPPED: ' partial)
    unexpected=$(count '^FAIL: unexpected skip' partial)
    if [ "$(count "^FAIL: unexpected skip.*: $harness_reason\$" partial)" -ne 0 ] || [ "$unexpected" -ne $((skipped - 1)) ]; then
        echo "FAIL: partial: $skipped SKIPPED lines, $unexpected unexpected skips"
        rc=1
    else
        echo "PASS: partial: only the harness's skip is expected ($unexpected unexpected skips)"
    fi
fi
# an invalid pattern: one error, before any section
if run invalid 2 FFMPEG_READ_TEST_STRICT=1 'FFMPEG_READ_TEST_EXPECTED_SKIPS=(no hap'; then
    if [ "$(count '^error: FFMPEG_READ_TEST_EXPECTED_SKIPS is not a valid' invalid)" -ne 1 ] ||
        [ "$(count '^SKIPPED' invalid)" -ne 0 ]; then
        echo "FAIL: invalid: no single error before the sections"
        rc=1
    else
        echo "PASS: invalid: an invalid pattern is an error (exit status 2)"
    fi
fi
exit "$rc"
