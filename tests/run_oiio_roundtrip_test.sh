#!/usr/bin/env bash
#
# OIIO read/write roundtrip sanity harness.
#
# SCOPE (be honest): This does NOT exercise the ReadOIIO/WriteOIIO OpenFX plugin
# code, which is embedded in the OFX plugin and needs an OFX host to drive (see
# README.md; the plugins currently cannot be built in this checkout because the
# vendored openfx submodule version does not match the source).
#
# What it DOES validate: that the OpenImageIO library and image formats our
# plugins depend on (EXR/TIFF/PNG) round-trip correctly in this environment,
# using the same library (OpenImageIO) that ReadOIIO/WriteOIIO wrap. It is a
# useful dependency/environment sanity check and a template for a future
# host-driven plugin test.
#
# Exit status: 0 = all roundtrips within tolerance, non-zero otherwise.
set -euo pipefail

if ! command -v oiiotool >/dev/null 2>&1; then
    echo "error: oiiotool not found (part of OpenImageIO)." >&2
    exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

ref="$tmp/ref.exr"
oiiotool --pattern constant:color=0.25,0.5,0.75 64x64 3 -o "$ref"

rc=0
roundtrip() { # name  outfile  bitdepth-args  fail-threshold
    local name="$1" out="$2" depth="$3" thresh="$4"
    oiiotool "$ref" $depth -o "$out"
    if oiiotool --fail "$thresh" --warn "$thresh" --diff "$ref" "$out" >/dev/null 2>&1; then
        echo "  PASS  $name roundtrip (<= $thresh)"
    else
        echo "  FAIL  $name roundtrip (> $thresh)"
        rc=1
    fi
}

echo "OIIO library roundtrip checks:"
roundtrip "EXR half/float" "$tmp/rt.exr" ""          0.0
roundtrip "TIFF uint16"    "$tmp/rt.tif" "-d uint16" 0.0002
roundtrip "PNG uint8"      "$tmp/rt.png" "-d uint8"  0.004

if [ "$rc" -eq 0 ]; then
    echo "ALL OIIO ROUNDTRIP CHECKS PASSED"
else
    echo "OIIO ROUNDTRIP CHECKS FAILED"
fi
exit "$rc"
