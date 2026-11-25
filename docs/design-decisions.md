# Design decisions

## epoll as the default

Level-triggered epoll reports a still-readable fd again. That forgives a bug where the loop stops early. Edge trigger is available and tested (`Echo.RoundTripLevelAndEdge`). The 2 second, 10-connection samples were 216320 msgs/s edge versus 209076 level, with a lower edge p99. One run each. Not enough to change the default.

## One connection, one thread

Sharing a connection across workers means a mutex on every read and write, or a proof that those accesses do not overlap. Thread confinement makes the proof the type of the code: only the worker runs the methods. Fan-out in pub/sub posts a task to the owner instead of touching the fd.

## Bounded queues

An unbounded task queue turns a slow consumer into unbounded memory. The handoff and the task queue both return false when full. The acceptor closes the fd. `try_send` returns `Backpressured`.

## Mutex queue vs SPSC

The accept path has one producer. A SPSC with acquire/release is enough, and the uncontended microbench was 1.12 ns versus 8.69 ns for the mutex queue. That microbench is not the server. Sends from arbitrary threads are multi-producer, so they use the mutex. The server is not lock-free.

## Pooling

Size-class freelists avoid a `malloc` per small frame on a worker that already paid for the blocks. They cost retained memory and a class-rounding waste. The cap makes the retention finite. No on/off benchmark was recorded, so this file does not claim the pool is faster.

## Readiness vs completion

epoll tells you a socket can take a syscall. You still call `recv` or `sendmsg`, and you must handle short transfers. io_uring's recv and send *are* the transfer. The buffer has to stay put until the CQE, including when the result is short, zero, or `-ECANCELED`.

## Affinity

Off by default. Pinning helps when you have measured migration, and this session did not get `perf` counters (`perf` missing, paranoid 2). The code still refuses to pin outside the current cpuset.

## Backpressure

Pausing reads is TCP's flow control applied one layer up: stop reading so the peer's window fills. Rejecting local `send` stops the application from queueing more than `max_pending_output`. Both are required. Pausing reads does not stop another thread from calling `send`.

## Connection lifetime

The fd is removed from epoll and closed before `on_disconnect`. Generation bumps after the callback, then the slot can be reused. An event already sitting in the current `epoll_wait` batch carries the old generation and is ignored.

## Benchmark limits

Same machine, short runs, one repeat of a single case. The blocking server won at 1 connection. Four workers on four vCPUs plus a 2-thread generator made the tail worse. Those results are the reason the README does not say the reactor is faster in general.
