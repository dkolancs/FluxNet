# Design decisions

## epoll as the default

Level-triggered epoll reports a still-readable fd again. That forgives a bug where the loop stops early. Edge trigger is available and tested (`Echo.RoundTripLevelAndEdge`). The release samples in `results/raw/` were 204479 msgs/s edge versus 207144 level, 10 connections, 64 bytes, 2 seconds, one run each. Edge p99 was lower (57.9 µs versus 79.9 µs). Not enough to change the default, and the throughput order flipped relative to an older unmerged run.

## One connection, one thread

Sharing a connection across workers means a mutex on every read and write, or a proof that those accesses do not overlap. Thread confinement makes the proof the type of the code: only the worker runs the socket methods. Pub/sub stores `ConnectionId` and calls `try_send`, which admits on the owner's slot and wakes that worker. It does not borrow a `Connection` from another worker. `on_disconnect` still receives the id so the topic map can drop it before the connection object is freed.

## Bounded queues

An unbounded task queue turns a slow consumer into unbounded memory. The handoff and the task queue both return false when full. The acceptor closes the fd. `try_send` returns `Backpressured`.

## Mutex queue vs SPSC

The accept path has one producer. A SPSC with acquire/release is enough. The uncontended microbench in `results/raw/queue-bench.json` was 1.17 ns versus 8.72 ns for the mutex queue. That microbench is not the server. Cross-thread sends admit under the slot mutex. The server is not lock-free.

## Pooling

Size-class freelists avoid a `malloc` per small frame on a worker that already paid for the blocks. They cost retained memory and a class-rounding waste. The cap makes the retention finite. The 10-connection, 64-byte pair was 207144 msgs/s with the pool and 205229 without (`epoll-1w-c10-p64.json` and `epoll-1w-c10-p64-nopool.json`). One run each. That is not a measured speedup. The pool stays as the bounded freelist; `--no-pool` remains for the comparison. Scatter/gather off on the same shape was 203398 msgs/s (`epoll-1w-c10-p64-nogather.json`). A 64-byte echo is one chunk, so gather has nothing extra to gather.

## Readiness vs completion

epoll tells you a socket can take a syscall. You still call `recv` or `sendmsg`, and you must handle short transfers. io_uring's recv and send *are* the transfer. The buffer has to stay put until the CQE, including when the result is short, zero, or `-ECANCELED`.

## Affinity

Off by default. `perf` is not installed and `perf_event_paranoid` is 2, so there are no counters. The 1-worker, 10-connection pair was 208064 msgs/s with `--affinity` and 207144 without. One run each. The default stays off. The code still refuses to pin outside the current cpuset.

## Backpressure

Pausing reads is TCP's flow control applied one layer up: stop reading so the peer's window fills. Rejecting local `send` stops the application from queueing more than `max_pending_output`. Both are required. Pausing reads does not stop another thread from calling `send`.

## Connection lifetime

The fd is removed from epoll and closed before `on_disconnect`. The outbound slot's generation is cleared before that callback, so a `try_send` during `on_disconnect` returns `Closed` and queues nothing. `Connection::id()` still returns the id for the callback, which is what pub/sub uses to unsubscribe. The connection's generation counter bumps after the callback, then the slot can be reused. An event already sitting in the current `epoll_wait` batch carries the old generation and is ignored.

## Benchmark limits

Same machine, short runs, one repeat of a single case. The blocking server won at 1 connection. Four workers on four vCPUs plus a 2-thread generator made the tail worse. Those results are the reason the README does not say the reactor is faster in general.
