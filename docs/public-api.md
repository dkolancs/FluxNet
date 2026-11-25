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

`Server::send`, `Server::try_send`, and the `Client` equivalents admit the frame before they return. They do not wait for the kernel to accept the bytes, and they do not wait on another worker's loop. `Accepted` means the full frame is queued and will be written, or the connection will surface a hard error or close. It is not a silent drop. `Backpressured` means nothing from that call was queued: the connection was already at `output_high_watermark`, or the frame would pass `max_pending_output`. `tests/integration/test_admission.cpp` blocks the owner inside `on_connect` and checks that `try_send` stops at the cap.

`SendResult` is `Accepted`, `Backpressured`, `Closed`, or `InvalidMessage`.

## Lifecycle

`run()` blocks. `stop()` is safe from another thread. Do not destroy the server from inside a callback. `wait_until_running` is how another thread learns that `bind` has finished. `bound_port()` is valid after that returns true. Port 0 asks the kernel for an ephemeral port.

States: `Starting`, `Running`, `Draining`, `Stopping`, `Stopped`.

## Configuration

See `NetConfig` in `include/fluxnet/config.hpp`. Defaults: 1 worker, epoll level trigger, 1 MiB frames, high watermark 256 KiB, low 64 KiB, max pending output 1 MiB, idle 60 s, slow consumer 5 s, shutdown 2 s, `TCP_NODELAY` on, affinity off. Zero disables a timer. `validate` throws `std::invalid_argument` if the watermarks are inconsistent.
