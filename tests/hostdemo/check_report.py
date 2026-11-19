#!/usr/bin/env python3
# Check the parameters and the renders in a hostDemo --report file.
#
# Usage: check_report.py <report.json> <check>...
# Checks:
#   value:<param>=<value>   the param's value after the run (at time 0)
#   keys:<param>=<keys>     the param's keys after the run, [[time, value], ...] ([] if none)
#   mean@<t>=<r,g,b,a>      the per-component mean of the output image of every render (each view) at time t,
#                           within 1e-4
# <value> and <keys> are JSON (e.g. 1, [[0,1],[1,3]]); a <value> that is not JSON is a string (e.g. x2).
# Exit status: 0 if all checks pass, 1 otherwise.
import json
import sys

TOLERANCE = 1e-4


def main():
    if len(sys.argv) < 3:
        print("Usage: check_report.py <report.json> <check>...")
        return 1
    with open(sys.argv[1]) as f:
        report = json.load(f)
    params = report.get("params") or {}
    renders = report.get("renders") or []
    ok = True
    for check in sys.argv[2:]:
        what, sep, expected = check.partition("=")
        if not sep:
            print("check_report: invalid check %r" % check)
            ok = False
            continue
        if what.startswith("value:") or what.startswith("keys:"):
            field, _, name = what.partition(":")
            try:
                value = json.loads(expected)
            except ValueError:
                value = expected
            if name not in params:
                print("check_report: no parameter %s" % name)
                ok = False
                continue
            got = params[name].get(field)
            if got != value:
                print("check_report: %s: %s %s, expected %s" % (name, field, json.dumps(got), json.dumps(value)))
                ok = False
        elif what.startswith("mean@"):
            t = float(what[len("mean@"):])
            mean = [float(v) for v in expected.split(",")]
            rs = [r for r in renders if r.get("time") == t]
            if not rs:
                print("check_report: no render at t=%s" % t)
                ok = False
            for r in rs:
                out = r.get("output")
                got = out.get("mean") if out else None
                if not got or len(got) < len(mean) or max(abs(got[c] - mean[c]) for c in range(len(mean))) > TOLERANCE:
                    print("check_report: t=%s view %s: mean %s, expected %s" % (t, r.get("view"), got, mean))
                    ok = False
        else:
            print("check_report: invalid check %r" % check)
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
