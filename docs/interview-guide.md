# Interview guide

This guide is about the FluxNet sources in this repository. It is not a script for claiming you wrote them. If you are walking someone through the code, say which files you are reading.

## The problem

TCP is a byte stream. A "message" is a convention on top. Clients send partial buffers, kernels return short writes, and a slow reader will fill your queue if you let it. FluxNet's job is to turn that stream into frames, on several threads, without unbounded memory and without sharing a connection's fd.

## How a frame gets to the callback

1. `Server::run` binds and listens (`src/core/server.cpp`).
2. `accept4` returns a non-blocking fd. The process-wide counter reserves a slot or the fd is closed.
3. The fd is pushed to a worker queue and the eventfd is written (`Worker::try_handoff_fd`).
4. The worker's epoll loop pops it, builds a `Conn`, and calls `on_connect` (`drain_handoff`).
5. `recv` fills a 16 KiB scratch. `FrameDecoder::feed` assembles a header, checks the length, then the payload (`src/protocol/codec.hpp`).
6. `on_message` runs on that worker. The view points at the decoder buffer.

Partial reads are the loop in `Worker::on_readable`: it keeps going until `EAGAIN`, EOF, an error, or a budget. A short `sendmsg` is `consume_output`, which advances an offset into the front buffer and only then pops it.

## Flow control, Nagle, buffers

`TCP_NODELAY` is on by default (`set_nodelay`). Nagle delays small sends to coalesce them. A request/response protocol usually does not want that delay. The cost is more packets. The socket send buffer is still the kernel's. FluxNet's output queue is the application's. The high watermark stops the application and, by pausing reads, pushes back on the peer's TCP window.

## epoll, poll, select

`select` and `poll` scan the whole set every call. epoll keeps the set in the kernel and returns what is ready. FluxNet uses epoll. Level trigger means "still ready." Edge trigger means "became ready." The code's answer to a budget-limited edge-triggered read is an explicit deferred list (`defer_read`), not a hope that the kernel will signal again.

## Readiness vs completion

epoll: the fd can make progress; you issue `recv`/`sendmsg`. io_uring recv/send: the completion *is* the progress, and the buffer must still be there (`src/backends/io_uring/uring.cpp`). Cancel is a completion too (`-ECANCELED` or a separate cancel CQE).

## eventfd, timerfd, signalfd

eventfd wakes a worker after a queue push. The consumer drains after the read and also before the next wait. timerfd wakes the same loop for idle and slow-consumer deadlines; the heap is lazy, and a stale node is ignored via generation. signalfd delivers `SIGINT`/`SIGTERM` on the acceptor epoll so the signal path is ordinary code. `SIGPIPE` is ignored because writes pass `MSG_NOSIGNAL`.

## Confinement, races, mutexes

Connection state is confined to one worker, so it does not need a mutex. The task queue does, because other threads produce. The SPSC does not, because the acceptor is the only producer and the worker is the only consumer. Indices are release-stored and acquire-loaded (`src/concurrency/spsc_ring.hpp`). They sit on separate 64-byte lines so the producer and consumer do not false-share. `static_assert` requires the atomic to be lock-free. That ring is not wait-free under the textbook definition in a useful way to claim here: a full queue fails the push, it does not wait, and the server around it takes mutexes.

A lost wakeup is why the push happens before the eventfd write, and why `stop()` now notifies the condition variable while holding its mutex. Both bugs are the same shape: the sleeper checked, then the waker signaled, then the sleeper slept.

## RAII and move

`FileDescriptor` closes in the destructor and is move-only. `Socket` and `PooledBuffer` are the same idea. Moved-from fds are -1. `close` is not retried.

## Latency

The histogram's percentile is a bucket upper bound. p99 on one connection was about 13 µs. p99 on 1000 connections was about 5.8 ms. Same server, same payload, different queueing. Tail latency moved because many connections shared one thread, not because the frame format changed. The 4-worker run improved rate and hurt the tail; it was also oversubscribed. Coordinated omission: a closed loop does not send the next request until the previous one returns, so it does not sample the waits you would see if you had offered a fixed rate during a stall. The generator can run open loop; the published matrix did not.

## Questions tied to the code

1. In `FrameDecoder::feed`, why is `payload_.resize` after the max-frame check? What happens on type 9?
2. Why does `update_epoll` add `EPOLLOUT` only when `has_output()` is true?
3. Why is `defer_read` called when reads resume, even in level trigger?
4. What does `try_send` returning `Accepted` not mean? Look at `Worker::send_external`.
5. Why is the pub/sub map not allowed to hold `Connection&`?
6. The blocking 1-connection server beat epoll. What cost is the reactor paying that the blocking loop is not?

## Exercises

- Turn `use_buffer_pool` off and on, rerun `scripts/benchmark.sh` for one case, and put both JSON files through `scripts/compare_benchmarks.py`.
- Run the 10-connection epoll case five times and report the spread. The two runs already in `results/raw` differ by about 4%.
- Set `worker_threads` to 1 and to 2 on this 4-vCPU guest with the same connection count, and compare p99. Do not add the generator's threads on top of the CPUs and then call the tail a property of epoll alone.
- Change the echo callback to copy the payload into a `std::string` stored on the connection and see that you no longer depend on `MessageView`'s lifetime. Measure whether it shows up at 64 bytes.

## Files

| Topic | File |
| --- | --- |
| Public send and callbacks | `include/fluxnet/server.hpp`, `src/core/connection.cpp` |
| Loop | `src/core/worker.cpp` |
| Frames | `src/protocol/codec.hpp` |
| SPSC | `src/concurrency/spsc_ring.hpp` |
| Pool | `src/memory/buffer_pool.cpp` |
| io_uring | `src/backends/io_uring/uring.cpp` |
| Numbers | `BENCHMARKS.md`, `results/raw/` |
