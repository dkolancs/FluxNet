# Benchmarks

## Methodology

All load is TCP to `127.0.0.1` on this guest. The generator and the server share the machine (4 vCPU, KVM, kernel 6.12.113+). Numbers describe this guest and this build, not a NIC and not a comparison with unpublished runs.

### Clocks and latency

The generator stamps `std::chrono::steady_clock` immediately before a successful send and subtracts it when the matching response is parsed. That is application RTT on loopback, including both stacks and scheduling.

`fluxnet::detail::LogHistogram` is a log2 histogram: `power = floor(log2(v))`, and 64 linear sub-buckets cover `[2^power, 2^(power+1))`. `percentile()` returns the **upper edge** of the bucket that crosses the percentile, so it does not under-report. Empty histograms report 0. This is not HdrHistogram.

When the generator uses more than one thread, each thread keeps its own histogram. Before p50, p95, p99, p99.9, and max are reported, `LogHistogram::merge` adds the bucket counts. The reported percentile is the percentile of the merged buckets. It is **not** the maximum of the per-thread percentiles. Throughput is the sum of completed messages. `Histogram.MergeIsNotMaxOfPercentiles` checks that those two methods disagree on a known pair of distributions.

Files under `results/legacy-unmerged/` were produced before that merge. Their percentiles are the max of the per-thread percentiles when the generator used more than one thread. They are not current results and they are not plotted.

### Closed loop

Every run below is closed loop with `--outstanding 1`. Open loop (`--rate`) is implemented and counts `behind_schedule`, but it was not part of the matrix. Payload bytes are a function of the seed, connection index, and sequence. A mismatch is a failure. Every run below had `failures=0`.

### What the JSON hash means

`fluxnet_bench` runs `git rev-parse --short HEAD`. These files say `ecc7a98`. That commit is the Asio baseline and the admission fix's parent on this branch. The binaries are the Release build of that commit. Later documentation edits do not change the measured binaries.

### How to reproduce

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++ -DFLUXNET_ENABLE_ASIO_BASELINE=ON
cmake --build build-release -j
DURATION=2 WARMUP=0.5 bash scripts/benchmark.sh
```

Compiler: g++ 13.3.0, `-O3 -DNDEBUG` via `CMAKE_BUILD_TYPE=Release`. liburing 2.5. CPU string from `/proc/cpuinfo`: `Intel(R) Xeon(R) Processor`. Boost 1.83.0 from the distro `libboost-dev` package, used only by `fluxnet_asio_echo`.

One run each unless a file name says otherwise. No confidence interval was computed. The older two-run epoll spread (about 4%) is in `results/legacy-unmerged/` and used the old percentile method, so it is not a confidence interval for this table.

## Results

| File | Server | conns | payload | workers / notes | msgs/s | p50 ns | p95 ns | p99 ns | p99.9 ns | max ns |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `epoll-1w-c1-p64.json` | epoll LT | 1 | 64 | 1 worker, 1 client thread | 79723 | 12288 | 13440 | 14976 | 23552 | 815396 |
| `blocking-c1-p64.json` | blocking, one conn at a time | 1 | 64 | 1 thread | 84672 | 11648 | 12672 | 13824 | 20992 | 216795 |
| `epoll-1w-c10-p64.json` | epoll LT, pool on, gather on | 10 | 64 | 1 worker | 207144 | 37888 | 75776 | 79872 | 99328 | 2564756 |
| `epoll-1w-c10-p64-nopool.json` | same, `--no-pool` | 10 | 64 | 1 worker | 205229 | 38400 | 77824 | 81920 | 104448 | 949652 |
| `epoll-1w-c10-p64-nogather.json` | same, `--no-gather` | 10 | 64 | 1 worker | 203398 | 38912 | 77824 | 81920 | 99328 | 1398196 |
| `epoll-1w-c10-p64-affinity.json` | same, `--affinity` | 10 | 64 | 1 worker | 208064 | 38400 | 76800 | 80896 | 97280 | 291293 |
| `epoll-et-1w-c10-p64.json` | epoll ET | 10 | 64 | 1 worker | 204479 | 47104 | 52736 | 57856 | 101376 | 206637 |
| `uring-1w-c10-p64.json` | io_uring workers, epoll acceptor | 10 | 64 | 1 worker | 221519 | 42496 | 52224 | 57344 | 76800 | 211377 |
| `threadpc-c10-p64.json` | thread per connection | 10 | 64 | one thread each | 198230 | 35840 | 65536 | 76800 | 892928 | 5816185 |
| `asio-c10-p64.json` | Boost.Asio 1.83, one `io_context` | 10 | 64 | not libfluxnet | 237147 | 38912 | 47616 | 50176 | 59392 | 168199 |
| `epoll-1w-c100-p64.json` | epoll LT | 100 | 64 | 1 worker, 2 client threads | 210717 | 471040 | 495616 | 557056 | 1245184 | 1638250 |
| `epoll-4w-c100-p64.json` | epoll LT | 100 | 64 | 4 workers, 2 client threads | 369486 | 149504 | 434176 | 4128768 | 6094848 | 15681901 |
| `epoll-4w-c10-p1024.json` | epoll LT | 10 | 1024 | 4 workers, 1 client thread | 185252 | 37888 | 71680 | 77824 | 101376 | 6306522 |
| `epoll-1w-c1000-p64.json` | epoll LT | 1000 | 64 | 1 worker, 2 client threads | 188600 | 5177344 | 6094848 | 6619136 | 6881280 | 6997247 |

The 100-connection and 1000-connection rows use 2 generator threads. Their percentiles are merged bucket counts.

### One variable at a time

Against `epoll-1w-c10-p64` (207144 msgs/s, p99 79872 ns):

- Pool off: 205229 msgs/s, p99 81920 ns. No benefit for the pool on this 64-byte sample. The pool stays; it is the bounded freelist, and one run is not a reason to delete it.
- Gather off: 203398 msgs/s, p99 81920 ns. A 64-byte echo is one chunk, so `sendmsg` with one iovec is not doing extra gathering. No benefit showed up. Gather stays on because multi-chunk output still uses it.
- Affinity on: 208064 msgs/s, p99 80896 ns. No benefit. The flag stays off. `perf` is not installed and `perf_event_paranoid` is 2, so this is not a counter-based comparison.

### Idle connections

`results/raw/scale-10000.txt` is a separate run, not a throughput test. `fluxnet_echo` with 1 worker, epoll, default `max_connections` 10000, held 10000 idle connections (`connected=10000 failed=0`). No `EMFILE` or `ENFILE` line was in the server log. While they were held, `/proc/<echo-pid>/status` showed `VmRSS` 177052 kB, `VmHWM` 177052 kB, `Threads` 2. The client sockets were in `fluxnet_hold`, not in that RSS. This is not 10000 actively messaging connections. The active case in this table stops at 1000 connections.

### Queue microbenchmark

`results/raw/queue-bench.json`, Google Benchmark 1.8.3, single thread, uncontended push then pop, `--benchmark_min_time=0.2s`:

| name | time |
| --- | --- |
| `spsc_ping` | 1.17 ns |
| `mutex_ping` | 8.72 ns |

This does not mean the server is faster with the SPSC. The server was not in this process. The SPSC is only the accept handoff.

### What these runs do not show

- 1M small messages/s was not reached. The highest FluxNet short run was 369486 msgs/s (4 workers, 100 connections, 64 bytes). The Asio 10-connection run was 237147 msgs/s, which is higher than 1-worker epoll on that shape and lower than the 4-worker FluxNet run.
- Sub-100 µs p99 held for 1 connection and for the 10-connection epoll, io_uring, thread-per-connection, and Asio runs. It did not hold for 100 connections on 1 worker (557 µs), 1000 connections (6.62 ms), or 4 workers and 100 connections (4.13 ms merged). The 4-worker process used more threads than the 4 vCPUs.
- On one connection, the blocking server was faster than epoll (84672 vs 79723 msgs/s).
- io_uring beat 1-worker epoll on the 10-connection case (221519 vs 207144). One run each. The acceptor was still epoll. Worker recv and send were io_uring.
- Asio was faster than 1-worker epoll on that same 10-connection shape (237147 vs 207144) with a lower p99 (50.2 µs vs 79.9 µs). One run. Asio used one `io_context` thread. It is not linked into `libfluxnet`.
- The later matrix in the next section did run one open-loop case and one churn case. A slow-read mode exists and is covered by `fluxnet_loadgen_test`, not by this table.
- `perf` is not installed. No hardware counters were collected.
- The 600 second measurement is in the soak section. A 1-hour or 24-hour soak was not started.

### Soak

`scripts/soak.sh` with `DURATION=600` wrote `results/raw/soak-600s.json`. The shell timestamps around that script were `2026-11-25T01:09:21Z` to `2026-11-25T01:19:22Z` (601 seconds of wall clock, including server startup and the generator's 1 second warmup). The JSON `duration_s` is 600. That is the measurement window. It is not a 1-hour or 24-hour soak. Those were not started.

| | |
| --- | --- |
| server | epoll LT, 2 workers |
| connections | 32 |
| payload | 64 |
| generator threads | 2 (percentiles are merged) |
| completed | 241170485 |
| failures | 0 |
| msgs/s | 401951 |
| p50 / p95 / p99 / p99.9 / max ns | 62464 / 128000 / 139264 / 188416 / 20179720 |
| payload bytes | 15434911040 |

`results/legacy-unmerged/soak-45s.json` is a 45 second run from before the histogram merge. It is not this soak.

## Later short matrix

`scripts/benchmark_matrix.sh` on the same guest, Release, duration 2 s, warmup 0.5 s, one server at a time. The measurement window starts after the generator finishes connecting. These are single samples except the three repeats. Failures were 0 in every file below.

| File | msgs/s | p99 ns | notes |
| --- | --- | --- | --- |
| `epoll-1w-c10-p256.json` | 189796 | 89088 | 1 worker, 10 conn, 256 B |
| `epoll-1w-c10-p4096.json` | 152776 | 94208 | 1 worker, 10 conn, 4096 B |
| `epoll-1w-c10-p65536.json` | 20503.5 | 532480 | 1 worker, 10 conn, 64 KiB |
| `epoll-2w-c100-p64.json` | 400792 | 442368 | 2 workers, 100 conn, 64 B, 2 client threads |
| `epoll-1w-c10-p64-r1.json` | 201612 | 82944 | repeat 1 of the 10-conn 64 B case |
| `epoll-1w-c10-p64-r2.json` | 203478 | 82944 | repeat 2 |
| `epoll-1w-c10-p64-r3.json` | 203446 | 83968 | repeat 3 |
| `epoll-1w-c10-p64-openloop.json` | 50000 | 52224 | `--rate 50000`, `behind_schedule` 983 |
| `epoll-1w-c4-p64-churn.json` | 191842 | 40960 | 4 conn, `--churn-seconds 0.5`, connect_failures 0 |
| `epoll-1w-c5000-p64.json` | 240632 | 58195968 | 1 worker, 5000 conn, 4 client threads, p50 12189696 ns |

Repeat msgs/s min 201612, median 203446, max 203478. That spread is about 1 percent. It is not a confidence interval for the other rows.

Eight workers were not run. `results/raw/workers-8-not-applicable.txt` records that this guest has 4 CPUs. 10000 active senders were not run after the 5000-connection row.

`results/raw/fault-cpu-before.json`, `fault-cpu-during.json`, and `fault-cpu-after.json` are a local `stress-ng --cpu 2` run, not CrashLab: 201611, 189836, and 202305 msgs/s, all with 0 failures. `tc` returned `EPERM`, and the Docker daemon socket was not created, so netem and compose were not run.

Charts: `results/plots/throughput.png`, `p99.png`, `latency_vs_connections.png` (names `epoll-1w-cN-p64`), `workers.png` (names `epoll-Nw-c100-p64`). `python3 scripts/plot_results.py` drew them from `results/raw` only.
