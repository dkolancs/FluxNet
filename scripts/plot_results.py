#!/usr/bin/env python3
"""Plot throughput from real results/raw/*.json. Exits 0 with a message if none exist."""
import json
import sys
from pathlib import Path

def main():
    root = Path(__file__).resolve().parents[1]
    raw = root / "results" / "raw"
    plots = root / "results" / "plots"
    files = sorted(p for p in raw.glob("*.json") if p.is_file())
    if not files:
        print("no JSON in results/raw; not drawing a chart")
        return 0
    rows = []
    for path in files:
        try:
            data = json.loads(path.read_text())
        except json.JSONDecodeError as exc:
            print(f"skip {path.name}: {exc}")
            continue
        if "msgs_per_s" not in data:
            continue
        rows.append((path.stem, float(data["msgs_per_s"]), float(data.get("p99_ns") or 0)))
    if not rows:
        print("JSON files have no msgs_per_s; not drawing a chart")
        return 0
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is not installed; not drawing a chart")
        return 0
    plots.mkdir(parents=True, exist_ok=True)
    names = [r[0] for r in rows]
    msgs = [r[1] for r in rows]
    fig, ax = plt.subplots(figsize=(max(8, len(names) * 0.7), 4.5))
    ax.bar(range(len(names)), msgs)
    ax.set_xticks(range(len(names)))
    ax.set_xticklabels(names, rotation=30, ha="right")
    ax.set_ylabel("completed messages / s")
    ax.set_title("FluxNet localhost runs (from results/raw)")
    fig.tight_layout()
    out = plots / "throughput.png"
    fig.savefig(out)
    print(f"wrote {out}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
