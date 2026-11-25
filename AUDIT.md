# Audit

Review of the tree as built in this session. Severity is about correctness of the library, not about missing features.

This is not a release recommendation. The library is a bounded reactor with tests and a few measured runs. It is not production-ready.

## Fixed in this session

### High — condition-variable lost wakeup on `stop()`

`Client::run` waits on a condition variable until `stop_flag` is set. `stop()` used to store the flag and call `notify_all` without holding the same mutex the waiter uses. A notify that lands after the predicate check and before the sleep is lost, and `run()` never returns. The echo test hung in `~RunningClient` until it was killed.

The same pattern existed on the server. Both now set the flag and notify while `mu` is held.

Regression: `Echo.RoundTripLevelAndEdge` returns (about 4 s for level and edge) instead of hanging. ASan, UBSan, and TSan integration runs also returned.

### High — `wait_until_running` treated the initial `Stopped` as completion

The predicate included `Stopped`. Before `run()` stored `Starting`, a caller could observe the initial state and return immediately. A following `stop()` then set a flag that `run()` cleared, and the acceptor loop ran with nobody waiting to stop it.

`startup_settled` is false until this `run()` reaches `Running` or fails back to `Stopped`. The smoke program that printed `wait ok=0 state=stopped` before `run enter` was that race.

### Medium — pub/sub test destroyed the broker while workers ran

TSan reported a data race: the main thread destroyed `Broker`'s maps while a worker was in `on_disconnect`. The maps are mutex-protected during callbacks. The destructor does not take that mutex, and it should not have to if the server has already joined. The test now `stop()`s and `join()`s the server before the broker is destroyed.

After that change, `ctest` in `build-tsan` passed 9/9. The race was in the example's lifetime, not in the SPSC or the reactor's connection ownership. It is still a real footgun: an application must not destroy objects captured by callbacks until `run()` has returned.

## Reviewed, not a defect

- `FileDescriptor` closes once and does not retry `close` after `EINTR`. On Linux the descriptor is released anyway. Documented in `DESIGN.md`. A unit test checks that a moved-from wrapper does not close the same number again.
- Epoll user data is `(index << 32) | generation`. Lookup ignores a stale generation. Slots are not recycled until generation bumps.
- SPSC indices use acquire/release, are on separate cache lines (`alignas(64)`), and `static_assert` that `atomic<size_t>::is_always_lock_free`. The queue is single-producer. Cross-thread sends use `MutexQueue`.
- Wake order is push, then `eventfd` write. The worker drains the queue at the start of the loop and again after the eventfd read.
- Frame length is checked against `max_frame_size` before `vector::resize`. A unit test feeds `0xffffffff` and expects `FrameTooLarge` without a huge allocation.
- io_uring recv and send buffers stay on the connection until the CQE. `io_uring_queue_exit` runs after a drain attempt. Leftover connections are only freed after the ring is gone.
- Output is rejected when `out_bytes` is already at the high watermark or the new frame would pass `max_pending_output`. Reads are paused at the high watermark and resumed at or below the low watermark, with an explicit deferred read so edge trigger does not wait for an edge that will not fire.
- Callback exceptions are caught in `feed` and `invoke_connect`. The connection is closed. The process stays up. `Echo.CallbackExceptionDoesNotKillServer` checks `state() == Running` and `callback_exceptions >= 1`.

## Remaining limits

### Medium — cross-thread `try_send` can drop after it returns `Accepted`

`Accepted` means the task was queued on the owner. If that worker later cannot enqueue because of the output cap, the frame is counted in `send_backpressured` and is not sent. Callers that need a hard answer must use the blocking `send` from a non-worker thread, or handle `on_backpressure`. This is documented. It is easy to misuse.

### Medium — benchmark percentiles with multiple generator threads are not merged

`fluxnet_bench` reports the max of per-thread percentiles. The 4-worker p99 of 4.13 ms is the worse thread, not a combined histogram. Throughput is summed and is fine. Do not quote that p99 as a single merged percentile.

### Low — acceptor is epoll even when workers are io_uring

Deliberate. Accept is not the data path. The README and `docs/io-backends.md` say so. A benchmark that says "io_uring" means worker recv/send.

### Low — one global log level and `SIGPIPE` ignored for the process

`run()` sets both. Two servers in one process share them.

### Low — `close` during `on_disconnect` is a no-op

`destroy_conn` sets `destroyed` before the callback so `send` returns `Closed`. That avoids re-entering destroy. Applications that need to flush from `on_disconnect` cannot.

### Not claimed

- No proof of absence of bugs beyond the tests and the sanitizer runs listed in `PROGRESS.md`.
- ASan was the default leak detector plus address checking. It reported no failures. That is not a proof about every shutdown path.
- TSan passed the current tests after the broker fix. It does not cover a long soak.
- The 45 second soak had 0 failures. It is not a durability test.

## Commands

```bash
ctest --test-dir build --output-on-failure          # 9/9, before clang-format
ctest --test-dir build-asan --output-on-failure     # 9/9
ctest --test-dir build-ubsan --output-on-failure    # 9/9
ctest --test-dir build-tsan --output-on-failure     # 9/9 after the broker lifetime fix
```

`clang-format -i` ran after those sanitizer ctests. After formatting, the debug integration binary was rebuilt and passed 10/10 (`fluxnet_integration`, about 14.5 s). Codec (5) and the SPSC thread tests (2) were also re-run. The sanitizer ctests themselves were not repeated after formatting.
