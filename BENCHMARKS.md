# Benchmarks

## Methodology

All load is TCP to `127.0.0.1` on this guest. The generator and the server share the machine (4 vCPU, KVM, kernel 6.12.113+). Numbers describe this guest and this build, not a NIC and not a comparison with unpublished runs.

### Clocks and latency

The generator stamps `std::chrono::steady_clock` immediately before a successful send and subtracts it when the matching response is parsed. That is application RTT on loopback, including both stacks and scheduling.

`fluxnet::detail::LogHistogram` is a log2 histogram: `power = floor(log2(v))`, and 64 linear sub-buckets cover `[2^power, 2^(power+1))`. `percentile()` returns the **upper edge** of the bucket that crosses the percentile, so it does not under-report. Empty histograms report 0. This is not HdrHistogram.

When the generator uses more than one thread, the reported percentile is the **maximum of the per-thread percentiles**, not a merged histogram. Throughput is the sum. The 4-worker and 1000-connection runs used 2 generator threads, so their percentiles are that maximum.

### Closed loop

Every run below is closed loop with `--outstanding 1`. Open loop (`--rate`) is implemented and counts `behind_schedule`, but it was not part of the matrix. Payload bytes are a function of the seed, connection index, and sequence. A mismatch is a failure. Every run below had `failures=0`.

### What the JSON hash means

`fluxnet_bench` runs `git rev-parse --short HEAD`. That printed `5e0a6aa`. That commit is the initial README only. The library sources were uncommitted because the configured git identity is `Cursor Agent` and this session did not commit. The numbers belong to the working tree that produced the binaries in `build-release`, not to the tree of `5e0a6aa`.

### How to reproduce

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++
cmake --build build-release -j
DURATION=2 WARMUP=0.5 bash scripts/benchmark.sh
```

Compiler: g++ 13.3.0, `-O3 -DNDEBUG` via `CMAKE_BUILD_TYPE=Release`. liburing 2.5. CPU string from `/proc/cpuinfo`: `Intel(R) Xeon(R) Processor`.

## Results

Single runs except where a second file is named. No confidence interval was computed.

| File | Server | conns | payload | workers / notes | msgs/s | p50 ns | p99 ns | p99.9 ns |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `epoll-1w-c1-p64.json` | epoll LT | 1 | 64 | 1 worker | 80518.5 | 12288 | 13440 | 19456 |
| `blocking-c1-p64.json` | blocking, one conn at a time | 1 | 64 | 1 thread | 84762 | 11392 | 14464 | 23040 |
| `epoll-1w-c10-p64.json` | epoll LT | 10 | 64 | 1 worker | 209076 | 39424 | 77824 | 99328 |
| `epoll-1w-c10-p64-repeat.json` | same | 10 | 64 | 1 worker | 217212 | 49664 | 68608 | (see file) |
| `epoll-et-1w-c10-p64.json` | epoll ET | 10 | 64 | 1 worker | 216320 | 43520 | 54272 | 80896 |
| `uring-1w-c10-p64.json` | io_uring workers, epoll acceptor | 10 | 64 | 1 worker | 223502 | 40448 | 56832 | 72704 |
| `threadpc-c10-p64.json` | thread per connection | 10 | 64 | one thread each | 206660 | 35328 | 72704 | 86016 |
| `epoll-1w-c100-p64.json` | epoll LT | 100 | 64 | 1 worker, 2 client threads | 214322 | 462848 | 614400 | 835584 |
| `epoll-4w-c100-p64.json` | epoll LT | 100 | 64 | 4 workers, 2 client threads | 373133 | 130048 | 4128768 | 6750208 |
| `epoll-4w-c10-p1024.json` | epoll LT | 10 | 1024 | 4 workers | 180128 | 39424 | 89088 | 141312 |
| `epoll-1w-c1000-p64.json` | epoll LT | 1000 | 64 | 1 worker, 2 client threads | 212232 | 4653056 | 5832704 | (see file) |
| `soak-45s.json` | epoll LT | 32 | 64 | 2 workers, 45 s, 2 client threads | 419925 | 58368 | 126976 | 167936 |

`soak-45s.json` completed 18896614 messages, 0 failures, 1209383296 payload bytes. It is a 45 second smoke soak. It is not a 10 minute, 1 hour, or 24 hour run.

### RSS

After the 1000-connection run, while `fluxnet_echo` (1 worker) was still up, `/proc/<pid>/status` showed `VmRSS` 21152 kB, `VmHWM` 21152 kB, `Threads` 2. `ps` showed about 96.4% CPU. That is one measurement, not a profile.

### Queue microbenchmark

`fluxnet_queue_bench` (Google Benchmark 1.8.3), single thread, uncontended push then pop:

| name | time |
| --- | --- |
| `spsc_ping` | 1.12 ns |
| `mutex_ping` | 8.69 ns |

This does not mean the server is 8x faster with the SPSC. The server was not in this process. The SPSC is only the accept handoff.

### What these runs do not show

- 10k connections were not attempted.
- 1M small messages/s was not reached. The highest short run was 373133 msgs/s (4 workers, 100 connections) and the 45 second run was 419925 msgs/s (2 workers, 32 connections).
- Sub-100 µs p99 held for 1 connection and for the 10-connection epoll/io_uring/thread-per-connection runs. It did not hold for 100 connections on 1 worker (614 µs) or 1000 connections (5.83 ms). The 4-worker 100-connection p99 was 4.13 ms; that process used more threads than the 4 vCPUs.
- On one connection, the blocking server was faster than epoll (84762 vs 80518 msgs/s). The reactor's overhead is visible when there is nothing to multiplex.
- io_uring beat 1-worker epoll by about 7% on the 10-connection case (223502 vs 209076). One run each. The acceptor was still epoll.
- Edge trigger was slightly ahead of level trigger on that same shape (216320 vs 209076) and its p99 was lower (54 µs vs 78 µs). One run each.
- The second 10-connection epoll run was 217212 vs 209076, about 4% higher. Treat the 2 second numbers as single samples.
- No Boost.Asio binary was built.
- `perf` is not installed. `scripts/profile.sh` printed `perf is not installed. No counters collected.`
- Open-loop, churn, and slow-consumer benchmark modes exist in the generator and were not part of this matrix. The slow-consumer *test* is in `tests/integration/test_limits.cpp` and does not publish a throughput number.

`python3 scripts/plot_results.py` wrote `results/plots/throughput.png` from the JSON files present at that time. The 1000-connection file and the repeat were added around the same period; re-run the script after adding files if the chart should include them.
