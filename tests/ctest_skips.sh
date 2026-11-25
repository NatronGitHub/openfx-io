#!/usr/bin/env bash
# ctest_skips.sh BUILD_DIR...: the skips that ctest does not show (#309). ctest --output-on-failure prints the output
# of the failing tests only, so the parts that a passing test skipped (e.g. the sections of test-ffmpeg whose encoder
# is missing) are invisible. This prints, for all tests of the last ctest run in each BUILD_DIR
# (BUILD_DIR/Testing/Temporary/LastTest.log), the skip lines of their output, then the number of such lines per test;
# in GitHub Actions, also one warning annotation per test with skips.
#
# A skip line starts with "SKIP: " or "SKIPPED: " (after optional blanks), followed by what is not run and why: the
# tests print one per skipped part. Other mentions of "skip" (e.g. a summary such as "3 plug-ins skipped") are not
# counted.
#
# Informative only: exit status 0, also when a BUILD_DIR has no LastTest.log (ctest did not run there); 2 on usage
# errors. The tests that ctest itself skipped (SKIP_RETURN_CODE) are in ctest's own "did not run" list.
set -euo pipefail

if [ "$#" -eq 0 ]; then
    echo "usage: $0 BUILD_DIR..." >&2
    exit 2
fi
for dir in "$@"; do
    log="$dir/Testing/Temporary/LastTest.log"
    if [ ! -f "$log" ]; then
        echo "$dir: no $log (ctest did not run)"
        continue
    fi
    echo "== skips inside the tests of $dir (from $log)"
    # LastTest.log: "N/M Test: NAME", then the output of NAME between "Output:" + a dashed line and "<end of output>"
    awk -v dir="$dir" -v ga="${GITHUB_ACTIONS:-}" '
        { sub(/\r$/, "") }
        /^[0-9]+\/[0-9]+ Test: / { name = $3; next }
        /^Output:$/ { out = 1; getline; next }
        /^<end of output>$/ { out = 0; next }
        out && /^[[:space:]]*SKIP(PED)?: / {
            if (!(name in n)) { order[++tests] = name }
            n[name]++
            print name ": " $0
        }
        END {
            if (tests == 0) { print dir ": no skips inside the tests" }
            for (i = 1; i <= tests; i++) {
                t = order[i]
                print dir ": " t ": " n[t] " skipped"
                if (ga == "true") {
                    printf "::warning title=Skips inside %s::%s: %s: %d skipped (see the step \"Skips inside the tests\")\n", t, dir, t, n[t]
                }
            }
        }' "$log"
done
