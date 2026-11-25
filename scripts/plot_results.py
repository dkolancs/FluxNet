#!/usr/bin/env python3
"""Charts from results/raw JSON only. Skips a chart when too few points exist."""
import json
import re
import sys
from pathlib import Path


def load_rows(raw: Path):
    rows = []
    for path in sorted(p for p in raw.glob("*.json") if p.is_file()):
        try:
            data = json.loads(path.read_text())
        except json.JSONDecodeError as exc:
            print(f"skip {path.name}: {exc}")
            continue
        if "msgs_per_s" not in data:
            continue
        rows.append((path.stem, data))
    return rows


def save_bar(path, labels, values, ylabel, title):
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(max(8, len(labels) * 0.55), 4.5))
    ax.bar(range(len(labels)), values)
    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(labels, rotation=35, ha="right")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    fig.tight_layout()
    fig.savefig(path)
    print(f"wrote {path}")


def save_line(path, xs, ys, xlabel, ylabel, title):
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(7, 4.5))
    order = sorted(range(len(xs)), key=lambda i: xs[i])
    ax.plot([xs[i] for i in order], [ys[i] for i in order], marker="o")
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    fig.tight_layout()
    fig.savefig(path)
    print(f"wrote {path}")


def main():
    root = Path(__file__).resolve().parents[1]
    raw = root / "results" / "raw"
    plots = root / "results" / "plots"
    rows = load_rows(raw)
    if not rows:
        print("no JSON in results/raw; not drawing a chart")
        return 0
    try:
        import matplotlib

        matplotlib.use("Agg")
    except ImportError:
        print("matplotlib is not installed; not drawing a chart")
        return 0
    plots.mkdir(parents=True, exist_ok=True)

    labels = [name for name, _ in rows]
    save_bar(
        plots / "throughput.png",
        labels,
        [float(data["msgs_per_s"]) for _, data in rows],
        "completed messages / s",
        "FluxNet localhost throughput (results/raw)",
    )
    save_bar(
        plots / "p99.png",
        labels,
        [float(data.get("p99_ns") or 0) / 1000.0 for _, data in rows],
        "p99 latency (us)",
        "FluxNet localhost p99 (results/raw)",
    )

    scale = []
    for name, data in rows:
        match = re.fullmatch(r"epoll-1w-c(\d+)-p64", name)
        if match and int(data.get("payload") or 0) == 64:
            scale.append((int(match.group(1)), float(data.get("p99_ns") or 0) / 1000.0))
    if len(scale) >= 2:
        save_line(
            plots / "latency_vs_connections.png",
            [item[0] for item in scale],
            [item[1] for item in scale],
            "connections",
            "p99 latency (us)",
            "epoll 1 worker, 64-byte payload",
        )
    else:
        print("skip latency_vs_connections.png: fewer than 2 matching files")

    workers = []
    for name, data in rows:
        match = re.fullmatch(r"epoll-(\d+)w-c100-p64", name)
        if match:
            workers.append((int(match.group(1)), float(data["msgs_per_s"])))
    if len(workers) >= 2:
        save_line(
            plots / "workers.png",
            [item[0] for item in workers],
            [item[1] for item in workers],
            "worker threads",
            "completed messages / s",
            "100 connections, 64-byte payload",
        )
    else:
        print("skip workers.png: fewer than 2 matching files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
