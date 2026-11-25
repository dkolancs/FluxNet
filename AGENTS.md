# FluxNet agent guide

FluxNet is a C++20 asynchronous TCP library for Linux. The core uses sockets, epoll, eventfd, timerfd, and signalfd. io_uring is an optional backend behind liburing. There is no Boost.Asio, libuv, or other networking framework inside the core.

## Objectives

- Correct, bounded, readable networking: RAII fds, one owner per connection, explicit backpressure.
- A public API small enough for an echo server in a few dozen lines.
- Tests that fail when ownership, framing, or shutdown is wrong.
- Benchmarks that record only numbers this machine actually produced.

## Scope

In scope: reactor, framing, thread confinement, bounded queues, buffer pools, timers, backpressure, shutdown, echo / pub-sub / in-memory KV examples, a load generator, optional io_uring, comparison baselines, docs, CI, Docker files.

Out of scope: a frontend, a database, AI integration, cloud infrastructure, a chosen LICENSE file, pushing to a remote, and any claim that the library is production-ready.

## Architecture constraints

- C++20, Linux, native syscalls. liburing is optional. GoogleTest and Google Benchmark are fetched at pinned tags.
- One acceptor thread and N worker threads. Each connection lives on one worker.
- Acceptor handoff is a bounded SPSC (or a mutex queue when configured). Application cross-thread work is a bounded mutex MPSC. Do not use the SPSC for multiple producers.
- Connection identity is `(worker, index, generation)`.
- `MessageView` is valid only inside `on_message` on the owning worker.
- Callback exceptions are caught. They close that connection only.
- Signals are delivered through signalfd. The handler path must stay async-signal-safe; the library blocks SIGINT/SIGTERM and reads signalfd on the acceptor.
- Logging is process-global, mutex-protected, and Warn by default so the hot path does not format logs.

## Coding standards

- Follow `.cursor/rules/cpp-style.mdc` and `.cursor/rules/ownership.mdc`.
- Prefer clear code over a claimed optimization. Do not say lock-free, zero-copy, or production-ready unless the specific path and the evidence support it.
- The SPSC ring is the only lock-free structure. The server as a whole is not lock-free.
- Buffer pools are worker-local. Returning a buffer to a pool is not a leak.
- Status vocabulary for features: Implemented and verified, Implemented but partially verified, Experimental, Blocked by environment, Not implemented.

## Thread and resource ownership

See DESIGN.md. Short version: the worker that accepts a connection (after handoff) is the only thread that may touch that connection's fd, buffers, and decoder. Other threads post a task to the worker's mutex queue and wake it with an eventfd.

## Testing

- Unit tests do not need the network except socket tests, which stay on localhost.
- Integration tests use port 0 and wait on condition variables with deadlines, not on an open-ended sleep.
- Sanitizer builds are separate directories: `build-asan`, `build-ubsan`, `build-tsan`.
- Do not weaken an assertion to hide a bug. Do not blanket-suppress sanitizers.

## Benchmark integrity

- Record methodology before results. Put raw JSON in `results/raw/`.
- Never invent a number. Never treat a smoke soak as a 10-minute or 24-hour soak.
- Load stays on 127.0.0.1.

## Git authorship

- Read the configured `user.name` and `user.email` before committing.
- The repo-local identity for this tree is `Divith Reddy Kolan <dkolan@asu.edu>`. Do not commit as Cursor, Claude, or another agent. Do not add Co-authored-by or Generated-by trailers. A machine hook may try to add one; do not leave it on the commit.
- Do not rewrite the initial commit. Do not push unless the user explicitly authorizes it.

## Definition of done

A feature is done when the code exists, the build covers it, a test or a recorded run exercises it, and the status line in PROGRESS.md matches what happened. A file that only compiles is not verification. A benchmark number without a raw result file is not a result.
