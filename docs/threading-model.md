# Threading

| Thread | Owns |
| --- | --- |
| `Server::run` | Listen socket, signalfd, acceptor epoll, joining workers |
| Worker i | Its connections, reactor, pool, timers, handoff consumer side, task queue consumer side |
| Any other thread | May call `stop`, `send`, `try_send`, `post`, `metrics` |

A connection's fd, decoder, and output queue are touched only by its worker. The acceptor pushes an fd into a bounded queue and writes the worker eventfd after the push. The worker pops until empty after observing the eventfd, and it also pops at the start of the loop so a push that races the wait is not stuck.

The SPSC ring is legal only for that handoff: one acceptor, one worker. `spsc_handoff = false` uses a mutex queue for the same job. Application tasks are always the mutex queue, because more than one thread may post.

`std::barrier` of `workers + 1` releases the acceptor only after every worker has set its thread id. `on_this_thread()` is that comparison. Cross-thread `send` admits into a per-slot mutex queue and wakes the owner with an eventfd. It does not block on the owner loop. The owner is still the only thread that calls `sendmsg` or submits an io_uring send. The slot object outlives the connection so the caller does not touch a `Conn` the owner might free.

Signals: `SIGINT` and `SIGTERM` are blocked with `pthread_sigmask` before workers start, and a signalfd is added to the acceptor epoll. The handler path is the epoll loop, not an asynchronous signal handler, so it may lock and log. `SIGPIPE` is ignored for the process because writes use `MSG_NOSIGNAL`.

Affinity is off by default. When on, worker `i` is pinned to `allowed[i % n]` from `sched_getaffinity`, which is the cpuset the process is actually allowed to run on.
