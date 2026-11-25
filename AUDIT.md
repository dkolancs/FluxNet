# Audit

Review of the tree after the admission fix, the merged histogram, and the sanitizer rerun. Severity is about correctness of the library, not about missing features.

This is not a release recommendation. The library is a bounded reactor with tests and measured runs. It is not production-ready. No confirmed critical correctness bug is still open in the paths below. That is not a proof that none exist.

## Fixed before this pass

### High — condition-variable lost wakeup on `stop()`

`Client::run` and `Server::run` wait on a condition variable. `stop()` used to store the flag and notify without holding the same mutex. A notify between the predicate check and the sleep was lost. Both now set the flag and notify while `mu` is held. `Echo.RoundTripLevelAndEdge` returns.

### High — `wait_until_running` treated the initial `Stopped` as completion

`startup_settled` stays false until this `run()` reaches `Running` or fails back to `Stopped`.

## Fixed in this pass

### High — `try_send` / `send` could return `Accepted` and then drop the frame

`Worker::send_external` queued a task and returned `Accepted` before the owner looked at the output cap. The task later called `enqueue`, which returned `Backpressured` and did not send. The caller had already been told the frame was accepted. The blocking `send` path had a related lie: a timeout returned `Backpressured` while the task could still queue the frame.

Admission is now `Worker::admit`. The frame is encoded into a `std::vector` and pushed onto a per-slot queue under that slot's mutex, or it is not pushed. `Accepted` means it was pushed. `Backpressured` means it was not. The owner thread is the only thread that copies the bytes into the buffer pool and writes the socket. A close may discard bytes that were already accepted; that close is the hard outcome.

Regression: `Admission.TrySendDoesNotAcceptPastCap`. The owner blocks inside `on_connect`. Sixteen `try_send` calls of 16-byte frames against `max_pending_output` 64 return exactly 4 `Accepted` and 12 `Backpressured`. `messages_out` matches the accepted count before the owner is released. The peer then reads 64 bytes.

Files: `src/core/worker.cpp`, `include/fluxnet/server.hpp`, `DESIGN.md`, `tests/integration/test_admission.cpp`.

### Medium — multi-thread percentiles were the max of per-thread percentiles

`fluxnet_bench` now calls `LogHistogram::merge` before p50/p95/p99/p99.9/max. `Histogram.MergeIsNotMaxOfPercentiles` uses 100 samples of 1000 and 100 samples of 100000. The merged p50 is below the max of the two p50s. Old JSON was moved to `results/legacy-unmerged/` and is not plotted. New files say the histogram method is merged bucket counts. The `git` field on those files is `ecc7a98`.

### Medium — `on_disconnect` could not see the connection id

`Connection::id()` returned empty when `destroyed` was set, and `destroy_conn` sets that flag before the callback. Pub/sub looked up an empty id and left the topic in the map. The id is now returned for the whole callback. The outbound slot generation is cleared first, so `try_send` during `on_disconnect` returns `Closed` and does not queue. `PubSub.DisconnectRemovesSubscription` expects `topics() == 0` after disconnect. TSan passed that test.

The broker still must not be destroyed while a worker is inside the callback. The test joins before the broker dies. That application rule is unchanged.

### Low — least-loaded dispatch had no test that diverged from round-robin

`Dispatch.LeastLoadedReturnsToIdleWorker` connects, closes, and connects again. Both loads are zero, so the scan keeps worker 0. Round-robin would have advanced. The test expects both connections on the same worker.

## Reviewed, not a new defect

- `FileDescriptor` closes once and does not retry `close` after `EINTR`.
- Epoll user data is `(index << 32) | generation`. Lookup ignores a stale generation.
- SPSC indices use acquire/release and are on separate cache lines. The queue is single-producer. Cross-thread sends use the slot mutex, not the SPSC.
- Wake order for handoff is still push, then `eventfd` write. Admitted output sets a dirty index under its own mutex, then writes the eventfd. `wait_timeout_ms` returns 0 while any slot is scheduled, so a missed wake does not sleep past the pending bytes.
- Frame length is checked against `max_frame_size` before payload allocation.
- io_uring recv and send buffers stay on the connection until the CQE. Partial sends consume `res` bytes and submit the rest. Cancel completions are counted. `io_uring_queue_exit` runs after a drain attempt. Accept stays on epoll; that is deliberate and untested as an io_uring operation because it was not implemented.
- `Echo.IoUringIfAvailable` requires `active_backend() == IoUring` and an empty `backend_note()` on this guest. It did not skip.
- `Echo.LargePayloadRoundTrip` echoes 256 KiB, which does not fit in one small write.
- Callback exceptions are caught. `Echo.CallbackExceptionDoesNotKillServer` still passes.
- `libfluxnet.a` has no Asio symbols. `fluxnet_asio_echo` is a separate binary.

## Commands after the fixes

```bash
ctest --test-dir build --output-on-failure       # 9/9, 38 gtest cases
ctest --test-dir build-asan --output-on-failure  # 9/9
ctest --test-dir build-ubsan --output-on-failure # 9/9
ctest --test-dir build-tsan --output-on-failure  # 9/9
```

Integration in each of those was about 15 seconds. No sanitizer suppression was added. The 600 second soak is not a TSan run.

## Remaining limits

### Low — bytes accepted and then discarded on close

If the connection closes before the owner writes, queued frames are dropped. The caller was told `Accepted`. The close or `on_disconnect` is the signal. A live connection does not drop an accepted frame to satisfy the cap.

### Low — acceptor is epoll even when workers are io_uring

Deliberate. `EMFILE` drops listen interest for 50 ms. That was not reimplemented as io_uring accept. A benchmark that says "io_uring" means worker recv/send.

### Low — one global log level and `SIGPIPE` ignored for the process

`run()` sets both. Two servers in one process share them.

### Low — `send` during `on_disconnect` returns `Closed`

The slot is closed to new admits before the callback so a send cannot observe a half-destroyed connection. Applications that need to flush from `on_disconnect` cannot.

### Not claimed

- No proof of absence of bugs beyond the tests, the sanitizer runs, the benchmark matrix, the 10000 idle connections, and the 600 second soak.
- ASan was address and leak checking on the test suite. It reported no failures. That is not a proof about every shutdown path.
- TSan passed the test suite, including cross-thread `try_send` while the owner was blocked. It did not run during the soak.
- 1M messages/s was not reached. The highest FluxNet figure in `results/raw/` is 401951 msgs/s over 600 seconds, and 369486 msgs/s on the 2 second 4-worker run.
- Single 2 second samples are not a confidence interval. The pool, gather, and affinity comparisons are one run each and were treated as noise.
- `perf`, Valgrind, libFuzzer, Docker, and CrashLab were not available. No counters, no fault-injection schema, and no container run are claimed.
