# Backpressure

A connection tracks `out_bytes`, the bytes queued for write, including headers.

`send` returns `Backpressured` and does not queue the frame when `out_bytes` is already at `output_high_watermark`, or when the new frame would exceed `max_pending_output`.

Crossing the high watermark removes read interest, calls `on_backpressure(connection, true)` once, and arms the slow-consumer timer. When `out_bytes` falls to `output_low_watermark` or below, reads resume, `on_backpressure(connection, false)` runs, and a read is queued explicitly. That last step matters for edge trigger: putting `EPOLLIN` back is not treated as a guaranteed new edge.

If the connection is still at the high watermark when the slow-consumer timer fires, it is closed. `slow_consumer_timeout` of 0 disables that. Idle timeout uses the last successful read or write and is also disabled by 0.

The accept path has its own bounds. If `max_connections` is hit, the fd is closed and `rejected_admission` increments. If the worker queue is full, the fd is closed and `dropped_handoff` increments. `EMFILE` or `ENFILE` from `accept` drops listen interest for 50 ms so a level-triggered epoll does not spin.

`tests/integration/test_limits.cpp` covers an oversized frame, the idle timer, the admission cap, and a client that does not read. The last test does not require `send_backpressured > 0`, because a kernel socket buffer can absorb a short burst. It does require that a connected server has not written an unbounded amount.
