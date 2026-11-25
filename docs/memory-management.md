# Memory

`BufferPool` is not shared. Each worker has one. Size classes are 1 KiB, 4 KiB, 16 KiB, and 64 KiB. A request picks the smallest class that fits. Larger requests are `new std::byte[n]` and are deleted on release, not retained.

Each class keeps at most `pool_max_per_class` free blocks (default 256). Release past the cap deletes the block. Blocks sitting on the freelist are intentional. ASan will not call them leaks if they are still reachable from the pool, and the pool deletes them in its destructor.

`PooledBuffer` is move-only. Destruction returns the memory. A cross-thread send first copies the encoded frame into a `std::vector` on the slot queue. The owner then copies that into a pooled buffer. Output frames live in a `deque` of those buffers until `sendmsg` or the io_uring CQE consumes them. The pool is not on the cross-thread path, because it is not thread-safe.

The recv scratch is a 16 KiB `vector` on the connection. The decoder copies a frame into its own vector so the scratch can be reused. That is a copy, not zero-copy.

`use_buffer_pool = false` allocates and deletes every buffer. No benchmark in `BENCHMARKS.md` compares the two. The pool unit test checks reuse and the cap.

Bounds that keep memory finite:

- `max_connections`
- `max_pending_output` per connection
- `max_frame_size` (also the largest task payload)
- `queue_capacity` tasks per worker
- freelist caps

The cross-thread task ceiling is `queue_capacity * max_frame_size` per worker. With the defaults that is 1024 * 1 MiB. The echo hot path does not use that queue, because `on_message` runs on the owner and `send` is inline.
