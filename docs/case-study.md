# Building FluxNet: Engineering a Low-Latency C++ Networking Framework from Scratch

This is an account of the tree in this workspace and the runs that produced `results/raw/`. It is not a story about a production launch. There are no users and no production deployment in this write-up.

## The machine

A KVM guest, kernel 6.12.113+, 4 vCPUs, about 16 GB RAM, no swap. `ulimit -n` was 524288. `perf` was still missing after apt could not find `linux-tools-6.12.113`, and `perf_event_paranoid` was 2. Docker's package installed, but the daemon did not start. `tc` returned EPERM. `stress-ng` and Valgrind did run. libFuzzer's runtime library was not installed. liburing 2.5 was present. Boost 1.83 headers were installed from `libboost-dev` so the Asio baseline could be compiled. CrashLab's repository is `github.com/dkolancs/CrashLab`. Its experiment schema was read. The matching YAML in this tree was not executed.

`/usr/bin/c++` was Clang and could not link `libstdc++`. `g++` 13.3.0 could. The builds used g++ on purpose.

## What had to be true before tuning

The first echo test hung. `stop()` set an atomic and notified a condition variable without holding the waiter's mutex, so the client thread slept forever in `run()`. A second bug made `wait_until_running` succeed immediately on the initial `Stopped` state, before `run()` had started, which then cleared the stop flag and left the acceptor up. Both are in `AUDIT.md`.

ThreadSanitizer then failed one test: the pub/sub broker was destroyed while a worker was still in `on_disconnect`. The test was changed to join the server first. A later audit found the callback itself could not unsubscribe: `Connection::id()` returned an empty id once `destroyed` was set, which happens before `on_disconnect`. The id is now valid for the callback. `PubSub.DisconnectRemovesSubscription` checks that the topic map drops to zero. TSan's 9 ctest binaries passed after that.

The send path had a worse contract bug. `try_send` returned `Accepted` when a task was queued. The worker could later hit the output cap and drop the frame, incrementing `send_backpressured` with no error returned to the caller. Admission now happens on a per-slot queue before `Accepted` is returned. If the cap would be exceeded, the result is `Backpressured` and nothing from that call is queued. The regression test blocks the owner inside `on_connect`, issues 16 frames against a 64-byte cap, and expects 4 accepted and 12 backpressured, then reads those 4 frames back.

The load generator used to report the max of per-thread percentiles. That is not a merged percentile. `LogHistogram::merge` adds bucket counts, and the JSON now says so. Files that used the old method were moved to `results/legacy-unmerged/` and are not the chart.

## What the numbers did

On one 64-byte connection, a blocking read/write loop did 84672 msgs/s and the 1-worker epoll server did 79723. The reactor was slower when there was only one socket. This write-up does not say the async server is faster.

At 10 connections the epoll server did 207144 msgs/s. Edge trigger was 204479. io_uring workers were 221519. Thread-per-connection was 198230. Asio, one `io_context`, was 237147. The gaps are one 2 second sample each. Turning the pool off was 205229, gather off was 203398, and affinity on was 208064. None of those single runs justified changing the defaults. `perf` still could not say why affinity did nothing.

Four workers and 100 connections reached 369486 msgs/s. The merged p99 was 4.13 ms. The generator used 2 threads, and the guest only has 4 CPUs. The tail got worse while the rate got better. A 600 second measurement with 2 workers and 32 connections did 401951 msgs/s, merged p50 62.5 µs, merged p99 139 µs, 0 failures, 241170485 messages. The script's wall clock was 601 seconds. That is the longest run. It is not a 1-hour or 24-hour soak.

1000 connections on 1 worker did 188600 msgs/s with a merged p50 of 5.18 ms and p99 of 6.62 ms. Separately, 10000 idle connections were held on 1 worker. The holder reported `connected=10000 failed=0`. The echo process RSS was 177052 kB and `Threads` was 2. There was no EMFILE in the log. Those connections were not sending. 1M small messages/s was not reached.

The SPSC microbenchmark was 1.17 ns versus 8.72 ns for an uncontended mutex push/pop. It was single-threaded and it was not the server. It supports "the handoff queue is cheap," not "the server is lock-free."

## What was left out on purpose

Registered buffers probed successfully and are not on the data path. Multishot recv returned EINVAL and is not used. The acceptor now submits `IORING_OP_ACCEPT` on its own ring when the worker backend is io_uring, and falls back to epoll if that ring fails. EMFILE still pauses accepts for 50 ms, via a timeout completion. No claim that the pool or affinity is faster. The CrashLab YAML matches the real schema and was not run, because Docker did not start and the probe is HTTP. A local `stress-ng` CPU run is in `results/raw/fault-cpu-*.json` and is labeled as not CrashLab. No license file. The README suggests MIT if someone adds one later.

A later 2 second matrix is in `BENCHMARKS.md`. Three repeats of the 10-connection 64-byte epoll case were 201612, 203446, and 203478 msgs/s. Five thousand active connections did 240632 msgs/s with p99 about 58 ms. That is not 1M msgs/s, and it is not 10000 active connections.

The benchmark JSON says git `ecc7a98`, which is the commit that contained the admission fix and the Asio baseline when the binaries were run.
