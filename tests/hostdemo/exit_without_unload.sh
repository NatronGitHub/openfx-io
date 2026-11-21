#!/bin/bash
# Run exit_without_unload (exit_without_unload.cpp) several times: a host that renders ReadOIIO, destroys its
# instance, and exits without kOfxActionUnload, must exit (#250). The hang it guards against is sporadic (MS
# Windows, at exit), hence the repeats.
#
# Usage: tests/hostdemo/exit_without_unload.sh <exit_without_unload> <dir with the bundle> [work dir]
# Environment:
#   OCIO                        default: tests/test.ocio
#   OIIOTOOL                    default: oiiotool, on PATH (writes the TIFF file)
#   EXIT_WITHOUT_UNLOAD_RUNS    runs (default 10)
#   EXIT_WITHOUT_UNLOAD_TIMEOUT seconds per run (default 120)
#   IO_HOSTDEMO_STRICT          1 (as in CI): a missing bundle or oiiotool fails instead of skipping
# Exit status: 0 on success, 1 on failure, 77 if the bundle or oiiotool is missing (1 with IO_HOSTDEMO_STRICT=1).
set -o pipefail

if [ $# -lt 2 ]; then
    echo "Usage: $0 <exit_without_unload> <dir with the bundle> [work dir]"
    exit 1
fi
abspath() { case "$1" in /*|[A-Za-z]:[/\\]*) printf '%s\n' "$1" ;; *) printf '%s\n' "$PWD/$1" ;; esac; }
PROG=$(abspath "$1")
PLUGINS=$(abspath "$2")
SRC=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(abspath "${3:-${TMPDIR:-/tmp}/openfx-io-exit-without-unload}")
RUNS=${EXIT_WITHOUT_UNLOAD_RUNS:-10}
TIMEOUT=${EXIT_WITHOUT_UNLOAD_TIMEOUT:-120}
STRICT=${IO_HOSTDEMO_STRICT:-0}

if command -v cygpath > /dev/null; then
    native() { cygpath -w "$1"; }
else
    native() { printf '%s\n' "$1"; }
fi
skipall() {
    if [ "$STRICT" = 1 ]; then
        echo "FAIL: $1 (IO_HOSTDEMO_STRICT=1)"
        exit 1
    fi
    echo "skip: $1"
    exit 77
}
ls -d "$PLUGINS"/*.ofx.bundle > /dev/null 2>&1 || skipall "no .ofx.bundle in $PLUGINS"
OIIOTOOL=${OIIOTOOL:-$(command -v oiiotool)}
[ -n "$OIIOTOOL" ] || skipall "oiiotool not found (set OIIOTOOL)"

rm -rf "$WORK"
mkdir -p "$WORK"
export OFX_PLUGIN_PATH HOME="$WORK" XDG_CACHE_HOME="$WORK" OCIO
OFX_PLUGIN_PATH=$(native "$PLUGINS")
OCIO=$(native "${OCIO:-$SRC/tests/test.ocio}")
if ! "$OIIOTOOL" --pattern checker:width=8:height=8 61x47 3 -d float -o "$(native "$WORK/rgb.tif")" < /dev/null > "$WORK/oiiotool.log" 2>&1; then
    cat "$WORK/oiiotool.log"
    echo "FAIL: oiiotool could not write rgb.tif"
    exit 1
fi

failed=0
for i in $(seq 1 "$RUNS"); do
    log="$WORK/run$i.log"
    (cd "$WORK" && exec "$PROG" fr.inria.openfx.ReadOIIO 2 "filename=$(native "$WORK/rgb.tif")") > "$log" 2>&1 &
    pid=$!
    # the watchdog is a sleep, a child of this shell: poll the program, and kill it if the watchdog ended first
    sleep "$TIMEOUT" &
    watchdog=$!
    hung=0
    while kill -0 $pid 2> /dev/null; do
        if ! kill -0 $watchdog 2> /dev/null; then
            hung=1
            kill -9 $pid 2> /dev/null
            if [ -r "/proc/$pid/winpid" ] && command -v taskkill > /dev/null 2>&1; then
                taskkill //F //T //PID "$(cat "/proc/$pid/winpid")" > /dev/null 2>&1
            fi
            break
        fi
        sleep 0.2
    done
    status=0
    wait $pid || status=$?
    kill $watchdog 2> /dev/null
    wait $watchdog 2> /dev/null
    if [ $hung -eq 1 ]; then
        echo "FAIL run $i: timeout (${TIMEOUT}s)$(grep -q '^exiting without' "$log" && echo ': hung at exit')"
        failed=$((failed + 1))
    elif [ $status -ne 0 ]; then
        echo "FAIL run $i: exit status $status"
        failed=$((failed + 1))
    else
        echo "ok   run $i"
        continue
    fi
    sed 's/^/     | /' "$log"
done
[ -s "$WORK/run1.log" ] && sed 's/^/     | /' "$WORK/run1.log"
echo "$((RUNS - failed)) of $RUNS runs exited"
[ $failed -eq 0 ]
