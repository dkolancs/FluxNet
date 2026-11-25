#!/usr/bin/env python3
"""Compare two FluxNet load-generator JSON files.

Refuses the comparison when host identity fields differ, because loopback
numbers from different machines are not a speedup.
"""
import argparse
import json
import sys


def load(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def pct(new, old):
    if old == 0:
        return None
    return (new - old) / old * 100.0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--max-drop", type=float, default=10.0, help="fail if msgs/s drop more than this percent")
    parser.add_argument("--max-p99-increase", type=float, default=25.0, help="fail if p99 grows more than this percent")
    args = parser.parse_args()
    base = load(args.baseline)
    cand = load(args.candidate)
    keys = ("cpu", "cores", "compiler")
    for key in keys:
        if base.get(key) != cand.get(key):
            print(f"refusing comparison: {key} differs ({base.get(key)!r} vs {cand.get(key)!r})")
            return 2
    if base.get("payload") != cand.get("payload") or base.get("connections") != cand.get("connections"):
        print("refusing comparison: payload or connection count differs")
        return 2
    b_msgs = float(base.get("msgs_per_s") or 0)
    c_msgs = float(cand.get("msgs_per_s") or 0)
    b_p99 = float(base.get("p99_ns") or 0)
    c_p99 = float(cand.get("p99_ns") or 0)
    print(f"msgs/s  baseline={b_msgs:.1f} candidate={c_msgs:.1f} change={pct(c_msgs, b_msgs)}")
    print(f"p99 ns  baseline={b_p99:.0f} candidate={c_p99:.0f} change={pct(c_p99, b_p99)}")
    print(f"failures baseline={base.get('failures')} candidate={cand.get('failures')}")
    failed = False
    drop = pct(c_msgs, b_msgs)
    if drop is not None and drop < -args.max_drop:
        print(f"FAIL throughput drop {drop:.1f}% exceeds {args.max_drop}%")
        failed = True
    growth = pct(c_p99, b_p99)
    if growth is not None and growth > args.max_p99_increase:
        print(f"FAIL p99 growth {growth:.1f}% exceeds {args.max_p99_increase}%")
        failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
