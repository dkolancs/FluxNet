# Benchmarking

Read `BENCHMARKS.md` before quoting a number. The method and the files are there.

The generator is `benchmarks/load_generator/main.cpp`. It is its own epoll client. It speaks the same 8-byte frames. It does not go through `fluxnet::Client`, so the numbers are not the public API's overhead. They are the server plus this generator on one machine.

Percentiles with more than one generator thread come from `LogHistogram::merge`. They are not the max of the per-thread percentiles. `results/legacy-unmerged/` holds the older files that used that max. Do not plot them as current results.

Closed loop keeps `--outstanding` requests in flight per connection. Open loop (`--rate`) schedules sends and increments `behind_schedule` when the schedule is late by more than two intervals. The published matrix is closed loop only.

Do not compare JSON files from different CPUs or different payloads. `scripts/compare_benchmarks.py` exits 2 when `cpu`, `cores`, `compiler`, `payload`, or `connections` differ.

`scripts/plot_results.py` reads `results/raw/*.json` and writes `results/plots/throughput.png`. If there is no JSON, it prints that and does not draw anything.

`scripts/profile.sh` exits without numbers when `perf` is missing or `perf_event_paranoid` is above 1. Both were true here.

`scripts/soak.sh` runs for exactly `DURATION` seconds of measurement, after a 1 second warmup. `DURATION=600` produced `results/raw/soak-600s.json`. The wall clock of that script was 601 seconds. A 45 second file under `results/legacy-unmerged/` is a 45 second run. A 1-hour or 24-hour invocation was not started.
