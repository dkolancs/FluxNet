# FluxNet design

Decisions below are the contract for the reactor. Later code should follow them. If an implementation detail diverges, this file and the code comment must be updated together.

## What this is

FluxNet serves length-prefixed TCP frames. A `Server` owns one acceptor thread and N worker threads. A `Client` owns N worker threads and no acceptor. Both are built on the same worker.

The public surface is `include/fluxnet/`. Callbacks see a `Connection` that is a non-owning view of the worker-owned `detail::Conn`. The view is only safe on the owning worker during the callback (or during a task that worker is running).

## Threading

- Acceptor thread: `Server::run()` itself. It binds, listens, accepts, and joins workers before it returns.
- Worker threads: each runs `Worker::thread_main`.
- A connection is created on a worker and never migrates.
- `Connection::send` and `Connection::close` are owner-thread operations. `Server::send`, `Server::try_send`, and the `Client` equivalents admit the frame on the slot queue before they return. They do not wait on the owner loop, because two workers blocking on each other would deadlock. The owner thread is still the only thread that writes the socket.
- Callbacks (`on_connect`, `on_message`, `on_disconnect`, `on_error`, `on_backpressure`) run on the owning worker.
- Exceptions leaving a callback are caught. The error is logged and that connection is closed.

## Reactors

Each worker has one backend for its lifetime:

- `Epoll` level-triggered (default) or edge-triggered (`EpollTrigger`).
- `IoUring` when liburing is compiled in and `io_uring_queue_init` succeeds.
- `Auto` tries io_uring, then epoll. The actual choice is `Server::active_backend()` plus `backend_note()`.

epoll interest:

- `EPOLLIN` while the connection is open, not read-paused, and not read-closed.
- `EPOLLOUT` only while output remains after a write that did not finish, or while a non-blocking connect is in progress.
- `EPOLLRDHUP`. Errors and hangups always close, after a final read attempt when `EPOLLIN` is also set.

Edge trigger rule: a read or write loop stops on `EAGAIN`, on a byte/iteration budget, or on pause. If the budget stops the loop before `EAGAIN`, the connection is placed on an explicit deferred list. The loop does not wait for another edge. When reads resume after backpressure, the connection is also deferred, because re-adding `EPOLLIN` is not treated as a guaranteed new edge.

io_uring is completion based. Each connection has at most one recv and one send submitted. The recv buffer and the send chunk stay allocated until the CQE. Close cancels in-flight ops and destroys the connection only when the in-flight count hits zero. This includes cancel completions. `io_uring_queue_exit` runs only after those CQEs are drained.

The acceptor uses epoll for the listen socket, the stop eventfd, and signalfd, including when workers run io_uring. Accept itself is a non-blocking `accept4` loop. That split is deliberate: one acceptor is not the data path, and the listen socket stays in level-triggered epoll so `EMFILE` can drop interest instead of spinning. Worker recv and send are the io_uring operations. If io_uring cannot be initialized, workers fall back to epoll and the note says why.

## Handoff and wakeups

Accept path, per worker:

1. Acceptor reserves a connection slot in the process-wide atomic counter (`fetch_add`). If that would exceed `max_connections`, it rolls back and closes the new fd.
2. Acceptor pushes the fd into that worker's queue. On failure it closes the fd, rolls back the reservation, and increments `dropped_handoff`.
3. Acceptor writes the worker eventfd **after** the push.

Consumer:

1. On eventfd readability, drain the counter.
2. Pop until the queue is empty.
3. Before the next blocking wait, pop again.

The queue is either a mutex deque or a lock-free SPSC ring (`spsc_handoff`, default true). The SPSC has a single producer (the acceptor) and a single consumer (the worker). Indices are `std::atomic<std::size_t>` with acquire/release. They are cache-line separated. Capacity is rounded up to a power of two. Full and empty use unsigned index subtraction, so all slots are usable.

Cross-thread `send` / `post` uses a mutex-protected deque bounded by `queue_capacity`. It is MPSC. It is not the SPSC.

## Connection identity

Slots live in a vector on the worker. A free list recycles indices. `generation` starts at 1 and increments on destroy, skipping 0. Epoll `data.u64` and io_uring user data carry index and generation. Lookup fails closed on a mismatch. File descriptors are removed from the reactor before `close`, and `close` is not retried after `EINTR` because on Linux the fd is already released and a retry can close a reused number.

## Buffers

`BufferPool` is worker-local and not thread-safe. Size classes are 1 KiB, 4 KiB, 16 KiB, and 64 KiB. Larger frames use a direct allocation that is not retained. The freelist for each class is capped by `pool_max_per_class`. Release past the cap deletes the block. `use_buffer_pool = false` allocates and deletes every time so a benchmark can compare.

A `PooledBuffer` is move-only RAII and returns its memory on destruction. Output chunks stay on a `std::deque` until `sendmsg`/`write` or the io_uring CQE consumes them. Input decoding copies into the decoder's payload buffer; the recv scratch can be reused immediately after `feed`.

Scatter/gather uses `sendmsg` with `MSG_NOSIGNAL` and up to 64 iovecs. `scatter_gather = false` sends only the first chunk per attempt (still correct, more syscalls).

## Protocol

8-byte header, network byte order, then payload:

| offset | field |
| --- | --- |
| 0 | u32 payload length (not including the header) |
| 4 | u16 type |
| 6 | u16 flags |

Types: `Data=1`, `Ping=2`, `Pong=3`, `Error=4`, `Close=5`. The library does not auto-answer ping. Unknown types and lengths above `max_frame_size` are protocol errors. The length is checked before any payload allocation. Bytes are copied with `memcpy`/`std::byte` arithmetic. Untrusted bytes are never cast to a struct.

`MessageView::payload()` points at the decoder buffer and is invalid after `on_message` returns. The callback may `send` or `close`. It must not re-enter the read loop.

## Send admission

This is the contract for `Connection::send`, `Server::send`, `Server::try_send`, and the `Client` equivalents.

- `Accepted` means the entire frame is queued for that connection. The owning worker will write it, or the connection will surface a hard error or close. Accepted is never a silent drop.
- `Backpressured` means nothing from that call was queued. The call returns `Backpressured` when `admitted + frame` would exceed `max_pending_output`, or when `admitted` is already at `output_high_watermark`.
- Admission is decided before the call returns. `try_send` does not return `Accepted` for a task the worker might later reject.
- The bytes live in a per-slot outbound queue on the worker (a mutex, a generation, and a byte count). The caller does not touch the `Conn` object. Only the owning worker moves bytes onto the socket and into the buffer pool.
- Cross-thread admission does not wait on the owner loop. Two workers sending to each other must not block on each other's reactors.
- If the connection closes before those bytes are written, they are discarded as part of that close. That is the hard outcome, not a drop on a live connection.
- `send` and `try_send` use the same admission. Neither waits for the kernel to accept the bytes.

`admitted` counts bytes queued and not yet accepted by the kernel, including frames the owner has not spliced into the socket queue yet.

## Backpressure and timers

- If `admitted + frame > max_pending_output`, or `admitted` is already at the high watermark, `send` / `try_send` returns `Backpressured` and does not queue the new frame.
- Crossing the high watermark removes read interest, fires `on_backpressure(conn, true)` once, and arms the slow-consumer timer.
- When `admitted` falls to the low watermark or below, reads resume, `on_backpressure(conn, false)` fires, and a read is deferred.
- A connection that stays at/above the high watermark for `slow_consumer_timeout` is closed. Zero disables the timer.
- Idle timeout is based on the last successful read or write. Zero disables it. Timers are lazy: one heap entry is armed, and a firing timer that is early is re-armed. Stale heap nodes are ignored via generation. They are popped when their deadline arrives, so churn cannot grow the heap without bound.
- `timerfd` uses a relative `CLOCK_MONOTONIC` deadline (`TFD` without `ABSTIME`) computed from `steady_clock`, which on libstdc++/libc++ Linux is `CLOCK_MONOTONIC`.

Per-connection read and write budgets (32 iterations or 256 KiB) keep one connection from monopolizing a worker. Leftover work is deferred.

## Shutdown

States: `Starting`, `Running`, `Draining`, `Stopping`, `Stopped`.

`stop()` is safe to call from another thread and more than once. It sets an atomic and writes eventfds. It does not take reactor locks and it is not called from a signal handler.

`SIGINT`/`SIGTERM` are blocked on the process threads started by `run`, and a signalfd on the acceptor observes them. `SIGPIPE` is ignored because `MSG_NOSIGNAL` covers socket writes; ignoring `SIGPIPE` also protects accidental writes.

On stop the acceptor closes the listen fd, sets drain, and records a deadline at `now + shutdown_timeout`. Workers stop accepting handoff, finish queued tasks (so blocking `send` waiters complete), flush or drop writes, close connections, and exit. `run()` joins them and returns. Callbacks are not invoked after the worker is destroyed. Destroying a `Server`/`Client` while `run()` is still on another thread waits until `Stopped` and aborts if that does not happen; destroying it from inside a callback can deadlock and is a contract violation.

## CPU affinity

Off by default. When enabled, the process cpuset (`sched_getaffinity`) is read and worker `i` is pinned to `allowed[i % allowed.size()]`. The acceptor uses the last allowed CPU. A CPU outside the cpuset is not requested.

## Logging and metrics

`Logger` writes to stderr under a mutex. Default level is `Warn`. Per-message success is not logged. Counters used from multiple threads are `std::atomic` with relaxed ordering. They are statistics, not synchronization.

## Memory bound (the one to defend)

A worker retains at most:

- one recv scratch (16 KiB) per connection,
- output bytes up to `max_pending_output` per connection,
- freelists capped per size class,
- at most `queue_capacity` pending cross-thread tasks, each holding one frame up to `max_frame_size`,
- at most `queue_capacity` handed-off fds.

Connection count is capped by `max_connections`. This is bounded, not tiny. `queue_capacity * max_frame_size * workers` is the cross-thread task ceiling and is documented so callers do not treat the defaults as free.

## Latency histograms

`LogHistogram::merge` adds bucket counts, the sample count, and the sum, and takes the minimum and the maximum. Percentiles are computed from the merged buckets. The load generator keeps a histogram per thread and merges them before it reports p50, p95, p99, p99.9, and max. It does not report the maximum of the per-thread percentiles. Throughput remains the sum of completed messages.

## What is intentionally not claimed

- The server is not lock-free.
- The pool is not zero-copy. Frames are copied into output buffers so the caller's memory and the `MessageView` can die when the callback returns.
- io_uring fixed buffers and registered files are not used.
- IPv6 is not implemented.
- Pub/sub and KV are examples on the public API, not part of the core.
