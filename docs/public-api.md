# Public API

Headers: `config.hpp`, `message.hpp`, `connection.hpp`, `metrics.hpp`, `server.hpp`, `client.hpp`.

## Callbacks

All callbacks run on the worker that owns the connection.

- `on_connect` after the TCP connection is usable.
- `on_message` for each decoded frame. `MessageView` dies when the callback returns.
- `on_disconnect` after the fd is removed from the reactor. `send` returns `Closed`.
- `on_error` for a connection that never reached connected, or for a protocol error.
- `on_backpressure` when reads pause or resume.

If a callback throws, the library catches it, logs at warn, and closes that connection. It does not stop the process.

## send

`Connection::send` is only for the owning worker. That is the callback and any task that worker is running.

`Server::send` / `Client::send` from a thread that is not a worker of this object queues the frame and waits up to `shutdown_timeout`. From a *different* worker it does not wait, because two workers blocking on each other can deadlock. `try_send` never waits. `Accepted` from `try_send` means the task was queued. The worker may still refuse it later if the output cap is hit; that shows up as `send_backpressured`.

`SendResult` is `Accepted`, `Backpressured`, `Closed`, or `InvalidMessage`.

## Lifecycle

`run()` blocks. `stop()` is safe from another thread. Do not destroy the server from inside a callback. `wait_until_running` is how another thread learns that `bind` has finished. `bound_port()` is valid after that returns true. Port 0 asks the kernel for an ephemeral port.

States: `Starting`, `Running`, `Draining`, `Stopping`, `Stopped`.

## Configuration

See `NetConfig` in `include/fluxnet/config.hpp`. Defaults: 1 worker, epoll level trigger, 1 MiB frames, high watermark 256 KiB, low 64 KiB, max pending output 1 MiB, idle 60 s, slow consumer 5 s, shutdown 2 s, `TCP_NODELAY` on, affinity off. Zero disables a timer. `validate` throws `std::invalid_argument` if the watermarks are inconsistent.
