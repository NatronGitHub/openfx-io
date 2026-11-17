#!/usr/bin/env python3
# Compare the images a reader rendered with those its writer rendered, from hostDemo's --report files.
#
# Usage: compare.py <reader.json> <writer.json> <tolerance> [--movie] [--crc]
#
# hostDemo reports, for each render, the per-component mean of the output image (output.mean, RGBA order). A writer
# copies its input to its output, so its renders are what it wrote; and it writes each render to the same file:
# - a still image file holds the writer's last render (t=1, right view): every render of the reader must match it;
# - a movie (--movie) holds one frame per time, the first view rendered (the left one, view 0): the reader's render at
#   time t is frame t - first (its time domain starts at first), i.e. the writer's render at that time, view 0.
# The output bounds must be the same, and no component mean may differ by more than <tolerance> (absolute, the images
# are in [0,1]). With --crc (lossless formats), the CRC-32 of the images (output.crc32) must also be the same, which
# catches flipped, mirrored or shifted images that have the same means. The reader's output must have no NaN
# (unwritten) pixel. Exit status: 0 if all match, 1 otherwise.
import json
import sys


def load(path):
    with open(path) as f:
        return json.load(f)


def main():
    if len(sys.argv) < 4:
        print("Usage: compare.py <reader.json> <writer.json> <tolerance> [--movie]")
        return 1
    reader = load(sys.argv[1])
    writer = load(sys.argv[2])
    tol = float(sys.argv[3])
    movie = "--movie" in sys.argv[4:]
    crc = "--crc" in sys.argv[4:]

    wrenders = [r for r in writer.get("renders", []) if r.get("output")]
    rrenders = [r for r in reader.get("renders", []) if r.get("output")]
    if not wrenders:
        print("compare: no render in the writer's report %s" % sys.argv[2])
        return 1
    if not rrenders:
        print("compare: no render in the reader's report %s" % sys.argv[1])
        return 1
    first = 0.
    if movie:
        td = reader.get("timeDomain") or {}
        rng = td.get("range")
        if not rng:
            print("compare: no time domain in the reader's report")
            return 1
        first = rng[0]

    ok = True
    for r in rrenders:
        t, view = r["time"], r["view"]
        out = r["output"]
        if out.get("nan"):
            print("compare: t=%s view %s: %s unwritten (NaN) pixels" % (t, view, out["nan"]))
            ok = False
        if movie:
            wt = t - first
            expected = [w for w in wrenders if w["time"] == wt and w["view"] == 0]
            if not expected:
                print("compare: t=%s view %s: the writer has no render at t=%s, view 0" % (t, view, wt))
                ok = False
                continue
            expected = expected[0]
        else:
            expected = wrenders[-1]
        rm = out["mean"]
        wm = expected["output"]["mean"]
        n = min(len(rm), len(wm))
        diff = max(abs(rm[c] - wm[c]) for c in range(n)) if n else float("inf")
        what = "t=%s view %s: mean %s, the writer's (t=%s view %s) %s" % (
            t, view, [round(v, 4) for v in rm], expected["time"], expected["view"], [round(v, 4) for v in wm])
        bad = []
        if out.get("bounds") != expected["output"].get("bounds"):
            bad.append("bounds %s, the writer's %s" % (out.get("bounds"), expected["output"].get("bounds")))
        if n == 0 or diff > tol:
            bad.append("means differ by %.4g > %g" % (diff, tol))
        if crc and (not out.get("crc32") or out.get("crc32") != expected["output"].get("crc32")):
            bad.append("crc32 %s, the writer's %s" % (out.get("crc32"), expected["output"].get("crc32")))
        if bad:
            print("compare: %s: %s" % (what, "; ".join(bad)))
            ok = False
        else:
            print("compare: %s: ok (%.2g%s)" % (what, diff, ", same crc32 %s" % out["crc32"] if crc else ""))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
