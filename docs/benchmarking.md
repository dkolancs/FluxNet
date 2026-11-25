# Benchmarking

Read `BENCHMARKS.md` before quoting a number. The method and the files are there.

The generator is `benchmarks/load_generator/main.cpp`. It is its own epoll client. It speaks the same 8-byte frames. It does not go through `fluxnet::Client`, so the numbers are not the public API's overhead. They are the server plus this generator on one machine.

Percentiles with more than one generator thread come from `LogHistogram::merge`. They are not the max of the per-thread percentiles. `results/legacy-unmerged/` holds the older files that used that max. Do not plot them as current results.

Closed loop keeps `--outstanding` requests in flight per connection. Open loop (`--rate`) schedules sends and increments `behind_schedule` when the schedule is late by more than two intervals. `--churn-seconds` closes and reconnects. `--slow-read-bytes` caps each `recv`. A non-loopback host is refused unless `--allow-remote` is set. The measurement window starts after connects finish. CSV is written next to the JSON.

One open-loop run is `results/raw/epoll-1w-c10-p64-openloop.json` (`--rate 50000`, `behind_schedule` 983, 50000 msgs/s, 0 failures). One churn run is `results/raw/epoll-1w-c4-p64-churn.json`.

Do not compare JSON files from different CPUs or different payloads. `scripts/compare_benchmarks.py` exits 2 when `cpu`, `cores`, `compiler`, `payload`, or `connections` differ. `scripts/test_compare_benchmarks.sh` checks that, and checks that a 20 percent throughput drop exits 1.

`scripts/plot_results.py` reads `results/raw/*.json` and writes `throughput.png`, `p99.png`, `latency_vs_connections.png`, and `workers.png` when enough matching files exist. It does not read `results/legacy-unmerged/`.

`scripts/profile.sh` writes `results/raw/perf.txt` and does not invent counters. On this guest `perf` was not installed and `perf_event_paranoid` was 2.

`scripts/soak.sh` runs for exactly `DURATION` seconds of measurement, after a 1 second warmup. `DURATION=600` produced `results/raw/soak-600s.json`. The wall clock of that script was 601 seconds. A 45 second file under `results/legacy-unmerged/` is a 45 second run. A 1-hour or 24-hour invocation was not started.
