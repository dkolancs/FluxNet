# FluxNet

FluxNet is a C++20 TCP library for Linux. The core uses sockets, epoll, eventfd, timerfd, and signalfd. io_uring is optional and is not linked unless liburing is found. There is no Boost.Asio or libuv in the core.

It is a teaching-sized reactor: one acceptor thread, N worker threads, one owner per connection, bounded queues, and explicit backpressure. It is not a claim of production readiness.

## What was measured here

These numbers are from one KVM guest (4 vCPU, kernel 6.12.113+, g++ 13.3.0, Release). The load generator and the server shared the machine. Loopback is not a NIC. The `git` field inside the JSON is `ecc7a98`. Raw files are in `results/raw/`. A bar chart of those files is `results/plots/throughput.png`. Percentiles are merged histograms when the generator used more than one thread. Older max-of-percentiles files are in `results/legacy-unmerged/` and are not this table.

| Run | msgs/s | p50 | p99 | failures |
| --- | --- | --- | --- | --- |
| epoll, 1 worker, 1 conn, 64 B, 2 s | 79723 | 12.3 µs | 15.0 µs | 0 |
| blocking, 1 conn, 64 B, 2 s | 84672 | 11.6 µs | 13.8 µs | 0 |
| epoll, 1 worker, 10 conn, 64 B, 2 s | 207144 | 37.9 µs | 79.9 µs | 0 |
| same shape, buffer pool off | 205229 | 38.4 µs | 81.9 µs | 0 |
| same shape, scatter/gather off | 203398 | 38.9 µs | 81.9 µs | 0 |
| same shape, CPU affinity on | 208064 | 38.4 µs | 80.9 µs | 0 |
| edge-triggered epoll, 1 worker, 10 conn, 64 B | 204479 | 47.1 µs | 57.9 µs | 0 |
| io_uring workers, 1 worker, 10 conn, 64 B | 221519 | 42.5 µs | 57.3 µs | 0 |
| thread-per-connection, 10 conn, 64 B | 198230 | 35.8 µs | 76.8 µs | 0 |
| Boost.Asio 1.83, one io_context, 10 conn, 64 B | 237147 | 38.9 µs | 50.2 µs | 0 |
| epoll, 4 workers, 100 conn, 64 B, client threads 2 | 369486 | 149.5 µs | 4.13 ms | 0 |
| epoll, 1 worker, 1000 conn, 64 B, client threads 2 | 188600 | 5.18 ms | 6.62 ms | 0 |
| 600 s measurement, epoll 2 workers, 32 conn, 64 B | 401951 | 62.5 µs | 139 µs | 0 |

The 1-connection blocking server beat the 1-worker epoll server on this guest. Four workers raised throughput and also raised tail latency; that run used more threads than the guest has CPUs. The p99 on that row is a merged histogram, not the max of the two client threads. 1M small messages/s was not reached. 10000 idle connections were held (RSS 177052 kB, 1 worker, no EMFILE). That is not 10000 active conversations. A later 5000-connection active run is in `results/raw/epoll-1w-c5000-p64.json` (240632 msgs/s, p99 58.2 ms, 0 failures). The 600 second row is the only soak that finished; it is not a 1-hour or 24-hour run.

Charts from `results/raw` are `results/plots/throughput.png`, `p99.png`, `latency_vs_connections.png`, and `workers.png`. Three repeats of epoll, 1 worker, 10 connections, 64 bytes were 201612, 203446, and 203478 msgs/s (min, median, max). Eight workers were not run: this guest has 4 CPUs.

## Architecture

```mermaid
flowchart LR
  acc[Acceptor thread<br/>io_uring accept or epoll]
  q1[Bounded queue + eventfd]
  q2[Bounded queue + eventfd]
  w1[Worker 0<br/>epoll or io_uring]
  w2[Worker N-1]
  acc --> q1 --> w1
  acc --> q2 --> w2
```

A connection is handed to one worker and stays there. Cross-thread `send` posts to that worker's mutex queue. The accept handoff can be a lock-free SPSC ring; that does not make the server lock-free.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/g++
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Release, ASan, UBSan, and TSan presets are in `CMakePresets.json`. On this machine `/usr/bin/c++` is Clang and does not find `libstdc++` unless `LIBRARY_PATH` includes the gcc library directory, so the known-good compiler is `g++`.

io_uring turns on when `pkg-config` finds liburing. Otherwise the build says so and the epoll backend remains. When workers use io_uring, the acceptor uses its own ring for `IORING_OP_ACCEPT` and falls back to epoll if that ring cannot be opened. `Server::accept_mode()` reports which one ran. Boost.Asio is a separate binary, `fluxnet_asio_echo`, built with `-DFLUXNET_ENABLE_ASIO_BASELINE=ON` when Boost headers are present. It is not linked into `libfluxnet`. On this machine that baseline was built against Boost 1.83 and the 10-connection row above is that run.

## Echo

```cpp
fluxnet::ServerConfig config;
config.host = "127.0.0.1";
config.port = 9000;
fluxnet::Server server(config);
server.set_on_message([](fluxnet::Connection& connection, fluxnet::MessageView message) {
    connection.send(message.type(), message.payload(), message.flags());
});
server.run();
```

`examples/echo`, `examples/pubsub`, and `examples/kv` are the three programs. Pub/sub stores connection ids, not connection objects, and fans out with `try_send`. The KV store is a mutex map with a byte cap.

## Reproduce the numbers

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++
cmake --build build-release -j
DURATION=2 WARMUP=0.5 bash scripts/benchmark.sh
python3 scripts/plot_results.py
```

`scripts/compare_benchmarks.py` refuses to compare JSON files whose CPU, core count, compiler, payload, or connection count differ.

## Tests

38 GoogleTest cases across 9 binaries: fd, socket, codec, queues, pool, histogram, integration (echo, limits, admission, pub/sub, KV), SPSC threads, and an in-process fuzz driver. Debug, ASan, UBSan, and TSan each reported 9/9 ctest binaries passed on this guest after the admission fix. libFuzzer and Valgrind were not available. The 600 second row above is the soak that finished. The script can take a longer `DURATION`; a 1-hour or 24-hour run was not started.

## Trade-offs

- Level-triggered epoll is the default because a missed edge is a stuck connection. Edge trigger is a config flag and reschedules explicitly when a budget stops the read before `EAGAIN`.
- The buffer pool is worker-local and capped. Retained blocks are not leaks. A flag turns the pool off.
- Output is capped. At the high watermark, reads pause and further `send` calls return `Backpressured`.
- io_uring submits one recv and one send per connection. The acceptor stays on epoll.

## Limitations

IPv4 only. No TLS, HTTP, or UDP. No Docker run (the daemon was not installed). No CrashLab repo was present; see `crashlab/INTEGRATION.md`. `perf` is not installed and `perf_event_paranoid` is 2. The pool, gather, and affinity switches were each measured once on the 10-connection shape and none of those single runs showed a benefit worth changing the defaults. Details and the host facts are in `LIMITATIONS.md`.

## License

No license file is included. If you add one, MIT is a reasonable default for a small library other people will read and copy: it is short and it requires attribution. Choose Apache-2.0 instead if you want an explicit patent grant. That choice is yours; this repository does not make it.

## More

`DESIGN.md` holds the invariants. `docs/` walks the protocol, the threads, and the benchmark method. `AUDIT.md` is the review of this tree. `PROGRESS.md` is what actually ran.
