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

When the resolved worker backend is io_uring, the acceptor opens its own ring before `run()` reports `Running`. `Server::accept_mode()` is `"uring"` in that case. The listen socket is blocking so `IORING_OP_ACCEPT` waits in the kernel instead of completing with `EAGAIN`. Accepted fds are still `SOCK_NONBLOCK | SOCK_CLOEXEC`. The first submission is multishot (`IORING_ACCEPT_MULTISHOT`). If that completion is `-EINVAL` or `-EOPNOTSUPP`, `accept_note()` records the errno and the loop rearms single-shot accepts. On this guest `accept_note()` stayed empty, so multishot accept was accepted by the kernel. `-EMFILE` and `-ENFILE` stop new accepts until a 50 ms `IORING_OP_TIMEOUT` completion. Shutdown posts `io_uring_prep_cancel64` for the accept user data, drains completions, closes any fd that arrives after cancel, then `io_uring_queue_exit`. If the acceptor ring cannot be created, `accept_mode()` is `"epoll"` and the original epoll loop runs. `backend_note()` stays empty when the worker backend itself was selected.

Worker operations are unchanged: at most one recv and one send, poll for connect/wake/timer, and cancel. A probe on this guest reported `register_buffers=ok recv_multishot=-22` (`-22` is `EINVAL`). Registered buffers are not used on the data path. Multishot recv is not used. Zero-copy send is not implemented.

`Echo.IoUringIfAvailable`, `Echo.IoUringAcceptMode`, and `Echo.IoUringShutdownWithAcceptInFlight` passed on this guest. The 10-connection benchmark `results/raw/uring-1w-c10-p64.json` was recorded before the acceptor moved to io_uring, so that file is worker io_uring with an epoll acceptor.

## What was not built

Multishot recv (probe returned `EINVAL`), registered buffers on the data path, registered files, and zero-copy send. A completion is not treated as a readiness event with a hidden `read` syscall; recv and send are the operations that move bytes.
