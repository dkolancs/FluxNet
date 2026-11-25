# I/O backends

## epoll

`EpollReactor` wraps `epoll_create1`, `epoll_ctl`, and `epoll_wait`. `EINTR` from `epoll_wait` is returned to the loop as zero events.

Interest:

- `EPOLLIN` while the connection is open, not read-paused, and not read-closed.
- `EPOLLOUT` only while output remains or a connect is in progress.
- `EPOLLRDHUP` always.

Level trigger is the default. Edge trigger adds `EPOLLET` and still stops a read or write at 32 iterations or 256 KiB. The connection is put on a deferred list and serviced on the next loop turn, with `epoll_wait` timeout 0 while that list is non-empty. Waiting for another edge would stall, because the data is already readable and no new edge will arrive.

`EPOLLERR` and `EPOLLHUP` close the connection after `SO_ERROR` is read. A read of 0 is EOF. If output remains, it is flushed, then the connection closes.

## io_uring

Compiled only with `FLUXNET_HAS_IO_URING`. `Auto` and `IoUring` call `io_uring_queue_init` at startup. Failure, or `io_uring_disabled != 0`, selects epoll and stores the `strerror` in `backend_note()`.

Each worker has its own ring (256 entries, halved twice if init fails). Per connection there is at most one `IORING_OP_RECV` and one `IORING_OP_SEND` (`MSG_NOSIGNAL`). Connect completion is `IORING_OP_POLL_ADD` for `POLLOUT`, then `SO_ERROR`. The wake eventfd and the timerfd are also `POLL_ADD`, re-armed after each completion.

Cancel uses `io_uring_prep_cancel64` on the op's user data. The connection is destroyed only when recv, send, and cancel completions have all been seen, or after `io_uring_queue_exit` if the drain times out (2 s). Buffers are members of the connection, so they outlive the CQEs that reference them.

The acceptor does not use the ring. It is epoll plus `accept4`. That is why an "io_uring" benchmark still has an epoll thread.

On this guest, `io_uring_queue_init(8)` in a probe returned features `0xffff`, and the echo integration test selected io_uring rather than skipping. The 10-connection benchmark is in `results/raw/uring-1w-c10-p64.json`.

## What was not built

Fixed buffers, registered files, multishot recv, and zero-copy send. A completion is not treated as a readiness event with a hidden `read` syscall; recv and send are the operations that move bytes.
