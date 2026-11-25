#!/bin/bash
# Run raw_unload (raw_unload.cpp) several times: a host that renders a DNG file with ReadOIIO (LibRaw, which uses
# OpenMP on Linux and MS Windows), unloads the plug-ins, keeps running, then exits (#239). Each run must not crash,
# the render must be demosaiced, and an OpenMP runtime loaded after the render must still be loaded after the unload
# (ReadOIIO pins it). With IO_HOSTDEMO_STRICT=1 on Linux and MS Windows, a run where no OpenMP runtime was loaded fails:
# the test would then exercise nothing (e.g. if LibRaw were built without OpenMP).
#
# Usage: tests/hostdemo/raw_unload.sh <raw_unload> <dir with the bundle> [work dir]
# Environment:
#   OCIO                 default: tests/test.ocio
#   RAW_UNLOAD_RUNS      runs (default 5)
#   RAW_UNLOAD_TIMEOUT   seconds per run (default 120)
#   OMP_NUM_THREADS      default 4: LibRaw's OpenMP regions use several threads
#   OMP_WAIT_POLICY      default active: the idle OpenMP workers keep running the runtime's code
#   IO_HOSTDEMO_STRICT   1 (as in CI): a missing bundle fails instead of skipping, and so does a run without an OpenMP
#                        runtime on Linux and MS Windows
# Exit status: 0 on success, 1 on failure, 77 if the bundle is missing (1 with IO_HOSTDEMO_STRICT=1).
set -o pipefail

if [ $# -lt 2 ]; then
    echo "Usage: $0 <raw_unload> <dir with the bundle> [work dir]"
    exit 1
fi
abspath() { case "$1" in /*|[A-Za-z]:[/\\]*) printf '%s\n' "$1" ;; *) printf '%s\n' "$PWD/$1" ;; esac; }
PROG=$(abspath "$1")
PLUGINS=$(abspath "$2")
SRC=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(abspath "${3:-${TMPDIR:-/tmp}/openfx-io-raw-unload}")
RUNS=${RAW_UNLOAD_RUNS:-5}
TIMEOUT=${RAW_UNLOAD_TIMEOUT:-120}
STRICT=${IO_HOSTDEMO_STRICT:-0}
# kill_hung: the stacks of a hung run, then kill it
. "$SRC/tests/hostdemo/hang.sh"

if command -v cygpath > /dev/null; then
    native() { cygpath -w "$1"; }
else
    native() { printf '%s\n' "$1"; }
fi
if ! ls -d "$PLUGINS"/*.ofx.bundle > /dev/null 2>&1; then
    if [ "$STRICT" = 1 ]; then
        echo "FAIL: no .ofx.bundle in $PLUGINS (IO_HOSTDEMO_STRICT=1)"
        exit 1
    fi
    echo "SKIP: no .ofx.bundle in $PLUGINS"
    exit 77
fi

rm -rf "$WORK"
mkdir -p "$WORK"
export OFX_PLUGIN_PATH HOME="$WORK" XDG_CACHE_HOME="$WORK" OCIO
OFX_PLUGIN_PATH=$(native "$PLUGINS")
OCIO=$(native "${OCIO:-$SRC/tests/test.ocio}")
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-4} OMP_WAIT_POLICY=${OMP_WAIT_POLICY:-active}
if ! "$PROG" --write-dng "$(native "$WORK/bayer.dng")" 640 480 > "$WORK/write-dng.log" 2>&1 || [ ! -s "$WORK/bayer.dng" ]; then
    cat "$WORK/write-dng.log"
    echo "FAIL: raw_unload --write-dng could not write bayer.dng"
    exit 1
fi

failed=0
for i in $(seq 1 "$RUNS"); do
    log="$WORK/run$i.log"
    rm -f "$WORK/run$i.stacks"
    (cd "$WORK" && exec "$PROG" fr.inria.openfx.ReadOIIO 2 --sleep 3 "filename=$(native "$WORK/bayer.dng")") > "$log" 2>&1 &
    pid=$!
    # the watchdog is a sleep, a child of this shell: poll the program, and kill it if the watchdog ended first
    sleep "$TIMEOUT" &
    watchdog=$!
    hung=0
    while kill -0 $pid 2> /dev/null; do
        if ! kill -0 $watchdog 2> /dev/null; then
            hung=1
            kill_hung $pid "$WORK/run$i.stacks"
            break
        fi
        sleep 0.2
    done
    status=0
    wait $pid || status=$?
    kill $watchdog 2> /dev/null
    wait $watchdog 2> /dev/null
    if [ $hung -eq 1 ]; then
        echo "FAIL run $i: timeout (${TIMEOUT}s)"
    elif [ "$STRICT" = 1 ] && [ "$(uname -s)" != Darwin ] && grep -q "^note: no OpenMP runtime" "$log"; then
        echo "FAIL run $i: no OpenMP runtime was loaded after the render (IO_HOSTDEMO_STRICT=1, not macOS)"
    elif grep -q "^FAIL" "$log"; then
        echo "FAIL run $i: $(grep -m 1 '^FAIL' "$log")"
    elif [ $status -ne 0 ]; then
        why="exit status $status"
        [ $status -gt 128 ] && why="$why (signal $((status - 128)))"
        grep -q "^threads:" "$log" && why="$why, after the unload"
        echo "FAIL run $i: $why"
    elif ! grep -q "^ok   render" "$log" || ! grep -q "^exiting after the unload: OK" "$log"; then
        echo "FAIL run $i: no render, or no exit message"
    else
        echo "ok   run $i: $(grep '^ok   demosaiced' "$log" | sed 's/^ok   //'); $(grep '^threads:' "$log"); $(grep '^plug-in binary' "$log")"
        grep -q "^note:" "$log" && echo "     $(grep '^note:' "$log")"
        continue
    fi
    failed=$((failed + 1))
    sed 's/^/     | /' "$log"
    [ -s "$WORK/run$i.stacks" ] && sed 's/^/     | /' "$WORK/run$i.stacks" | head -200
done
echo "$((RUNS - failed)) of $RUNS runs rendered the DNG file, unloaded IO.ofx and exited (OMP_NUM_THREADS=$OMP_NUM_THREADS)"
[ $failed -eq 0 ]
