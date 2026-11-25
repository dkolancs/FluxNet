# FluxNet plan

Status values: Implemented and verified, Implemented but partially verified, Experimental, Blocked by environment, Not implemented.

The living status is PROGRESS.md. This file is the roadmap and stays even when a phase is unfinished.

## Phase 0 — environment and contracts

Record OS, CPU, RAM, toolchain, io_uring, perf, docker, git identity, CrashLab search. Write AGENTS.md, rules, DESIGN.md, LIMITATIONS.md, BENCHMARKS.md methodology, CMake with C++20, Ninja, sanitizer presets, FetchContent GoogleTest (v1.14.0) and Google Benchmark (v1.8.3). Public headers under `include/fluxnet/`.

## Phase 1 — sockets and blocking baseline

Move-only `FileDescriptor`. `Socket` with bind/listen/accept4/connect/send/recv/shutdown, nonblocking, `TCP_NODELAY`, `SO_REUSEADDR`, optional `SO_REUSEPORT`, `SO_ERROR`, `MSG_NOSIGNAL`, `EINTR`/`EAGAIN`/`EOF`/`ECONNRESET`/`EPIPE`/`EMFILE`. Blocking echo and thread-per-connection echo that speak the same frames. Unit tests. Baseline numbers only after a real run.

## Phase 2 — epoll reactor

Level then edge via config. Register/modify/remove. Read, write, error, HUP, RDHUP. Batched `epoll_wait`. eventfd stop. ET drain until `EAGAIN`, with an explicit reschedule when the budget stops early. `EPOLLOUT` only while output is pending. Generation-checked connections.

## Phase 3 — binary protocol

8-byte big-endian header. Incremental decoder: split header, split payload, multiple frames, empty payload, max size before allocation, bad type, huge length. In-process deterministic fuzz. libFuzzer only if present.

## Phase 4 — multithreaded reactor

Acceptor thread, round-robin and least-loaded, admission limit, startup barrier, coordinated shutdown. Mutex queue and lock-free SPSC with acquire/release, padding, full/empty/wrap tests. SPSC is handoff only. Microbenchmark mutex vs SPSC.

## Phase 5 — memory

Worker-local pools, 1/4/16/64 KiB, bounded freelists, RAII handles, direct alloc above 64 KiB. `sendmsg` scatter/gather. Flag to disable the pool and to disable gather so the comparison is the same server.

## Phase 6 — backpressure and timers

High/low watermarks, max pending output, max input, max frame, idle timeout, slow-consumer timeout. timerfd + lazy heap. Tests for slow readers, oversized frames, stalls.

## Phase 7 — shutdown and logging

Lifecycle states. signalfd for SIGINT/SIGTERM. Stop accept, drain until deadline, join, release. Tests for stop under load and with pending writes.

## Phase 8 — public API and echo example

Echo server on the public API. Document callback thread, view lifetime, exceptions, shutdown.

## Phase 9 — io_uring

Optional. Real recv/send submissions, poll completions for wake and timer, cancel, buffers held until CQE, SQ-full retry, runtime detection, epoll fallback with the exact error. Benchmark against epoll only if both run.

## Phase 10 — CPU affinity

Config flag, default off. Honor the cpuset. No perf counters if `perf` is missing or `perf_event_paranoid` blocks them.

## Phase 11 — pub/sub and KV

Examples on the public API with in-process integration tests. Topic map is mutex-protected and stores connection ids, not pointers. Fan-out posts to the owning worker. KV is a mutex map with a byte cap.

## Phase 12 — benchmark client

Localhost load generator: closed loop, fixed-rate open loop, warmup, histogram, JSON/CSV, environment metadata, offered vs success vs fail. Histogram is a log2 histogram with 64 sub-buckets; percentiles use the bucket upper bound.

## Phase 13 — comparison matrix

Only variants that actually run, one factor changed at a time. Targets in the prompt (10k connections, 1M small msgs/s, sub-100 µs p99) are goals, not claims.

## Phase 14 — CrashLab

No CrashLab tree was found. Do not invent its schema. Document the boundary. `compose.yaml` runs echo and the load generator only.

## Phase 15 — tests

Unit, integration, concurrency, fuzz, a short stress test, sanitizer builds. Soak script with an explicit duration. A run counts only if it finished.

## Phase 16 — CI and regression tools

GitHub Actions for gcc/clang, debug/release, asan+ubsan job, tsan, clang-format. Benchmark job informational. `scripts/compare_benchmarks.py`, `benchmark.sh`, `profile.sh`, `plot_results.py`.

## Phase 17 — docs

README and `docs/` must match the code and the numbers that were measured.

## Phase 18 — adversarial review

AUDIT.md with severity, evidence, and what was retested. Not release-ready if a critical correctness bug is still open.

## Parallelism

This session is a subagent and does not spawn further agents. File ownership stays with this session so the reactor, the queues, and the tests agree.
