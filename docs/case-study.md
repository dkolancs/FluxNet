# Building FluxNet: Engineering a Low-Latency C++ Networking Framework from Scratch

This is an account of the tree in this workspace and the runs that produced `results/raw/`. It is not a story about a production launch.

## The machine

A KVM guest, kernel 6.12.113+, 4 vCPUs, about 16 GB RAM, no swap. `ulimit -n` was 524288. `perf` was missing and `perf_event_paranoid` was 2. Docker was not installed. liburing 2.5 was installed during the session because it was not there at the start, and a probe of `io_uring_queue_init(8)` succeeded. Boost.Asio was not installed. No CrashLab checkout existed.

`/usr/bin/c++` was Clang and could not link `libstdc++`. `g++` 13.3.0 could. The builds used g++ on purpose.

## What had to be true before tuning

The first echo test hung. `stop()` set an atomic and notified a condition variable without holding the waiter's mutex, so the client thread slept forever in `run()`. A second bug made `wait_until_running` succeed immediately on the initial `Stopped` state, before `run()` had started, which then cleared the stop flag and left the acceptor up. Both are in `AUDIT.md`. They showed up as a process in `futex_wait` and as a smoke program that printed `state=stopped` before `run enter`.

ThreadSanitizer then failed one test: the pub/sub broker was destroyed while a worker was still in `on_disconnect`. The fix was to join the server before destroying the map. After that, TSan's 9 ctest binaries passed. ASan and UBSan had already passed.

Those bugs are why the design notes insist on "enqueue, then wake" and on not destroying callback state until `run()` returns. The same class of mistake, lost wakeup, shows up with eventfd and with condition variables.

## What the numbers did

On one 64-byte connection, a blocking read/write loop did 84762 msgs/s and the 1-worker epoll server did 80518. The reactor was slower when there was only one socket. That is a reasonable result: epoll, a thread hop, and a frame copy cost more than `read` and `write` in a tight loop. It is also why this write-up does not say "the async server is faster."

At 10 connections the epoll server did about 209k–217k msgs/s across two runs, in the same band as a thread-per-connection baseline (206660). Edge trigger was 216320. io_uring workers were 223502. The gaps are a few percent on a 2 second sample. The second epoll run differed from the first by about 4%. That is the variability we actually saw, not a computed interval.

Four workers and 100 connections reached 373133 msgs/s and a p99 of 4.13 ms. The generator used 2 threads, so that p99 is the worse thread's p99, and the guest only has 4 CPUs. The tail got worse while the average rate got better. A 45 second run with 2 workers and 32 connections did 419925 msgs/s, p50 58 µs, p99 127 µs, 0 failures, 18896614 messages. That is the longest run. It is not a 10 minute soak.

1000 connections on 1 worker stayed near 212k msgs/s but p50 moved to 4.65 ms. RSS of that echo process was 21152 kB. 10k connections were not tried. 1M msgs/s was not reached.

The SPSC microbenchmark was 1.12 ns versus 8.69 ns for an uncontended mutex push/pop. It was single-threaded and it was not the server. It supports "the handoff queue is cheap," not "the server is lock-free."

## What was left out on purpose

No fixed io_uring buffers. No claim that the pool is faster, because that comparison was not run. No affinity before/after, because `perf` could not supply a reason to pin. No Asio chart, because the headers were not on the machine. No CrashLab report, because there was no schema to match. No license file.

The git identity on the machine is `Cursor Agent <cursoragent@cursor.com>`. The session did not commit. The benchmark JSON says git `5e0a6aa`, which is the initial commit, not this tree.
