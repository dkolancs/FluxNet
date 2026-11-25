# Progress

## Authorship

This completion branch is `cursor/fluxnet-completion`, cut from `origin/main` at `54f1df7`. Repo-local `user.name` is `Divith Reddy Kolan` and `user.email` is `dkolan@asu.edu`. Nothing in this pass was pushed. The initial commit still has a historical Co-authored-by line and was not rewritten.

## Feature status

| Feature | Status |
| --- | --- |
| C++20 CMake, Ninja, Debug/Release, ASan, UBSan, TSan | Implemented and verified |
| RAII `FileDescriptor`, TCP socket helpers | Implemented and verified |
| 8-byte frame codec, incremental decode, in-process fuzz | Implemented and verified |
| Epoll reactor, LT and ET, generation tokens, eventfd wake | Implemented and verified |
| Acceptor + N workers, round-robin and least-loaded, admission cap | Implemented and verified (`Dispatch.LeastLoadedReturnsToIdleWorker` plus the existing cap test) |
| Mutex queue and lock-free SPSC handoff | Implemented and verified |
| Worker-local buffer pool and `sendmsg` gather | Implemented and verified. On/off runs are in `BENCHMARKS.md`. Neither single 2 second sample showed a benefit. Defaults unchanged |
| Watermarks, idle timeout, slow-consumer timeout, timerfd | Implemented and verified |
| Send admission before `Accepted` | Implemented and verified (`Admission.TrySendDoesNotAcceptPastCap`) |
| signalfd shutdown, lifecycle states | Implemented and verified |
| Public server/client, echo example | Implemented and verified |
| io_uring worker recv/send/cancel plus acceptor `IORING_OP_ACCEPT` | Implemented and verified (`Echo.IoUringIfAvailable`, `Echo.IoUringAcceptMode`, `Echo.IoUringShutdownWithAcceptInFlight`). Multishot accept was accepted on this guest (`accept_note` empty). Multishot recv probe returned EINVAL and is not used. Epoll accept remains the fallback if the acceptor ring fails |
| CPU affinity flag, default off | Implemented and verified as "no benefit on one 10-connection run". Flag stays off. No perf counters |
| Pub/sub and KV examples | Implemented and verified. Disconnect removes the subscription. TSan passed |
| Load generator, merged histogram, JSON | Implemented and verified. Old unmerged files are in `results/legacy-unmerged/` |
| Blocking and thread-per-connection baselines | Implemented and verified (2 s runs after the histogram fix) |
| Boost.Asio baseline | Implemented and verified as an external binary. One 10-connection run. Not linked into libfluxnet |
| 10000 idle connections | Implemented and verified. `connected=10000 failed=0`, RSS 177052 kB |
| 10000 active connections | Implemented and verified. `results/raw/epoll-1w-c10000-p64.json`: 439820 msgs/s, p99 40370176 ns, failures 0, connect_failures 0. Echo was started with `--max-connections 20000` |
| 600 second soak | Implemented and verified. Wall clock of the script 601 s. `duration_s` 600 |
| 1 hour soak | Implemented and verified. `results/raw/soak-3600s.json`. Script log `/tmp/fluxnet-soak-1h.log` from `2026-11-25T03:00:51Z` to `2026-11-25T04:00:52Z` (EXIT 0). duration_s 3600, completed 1411841756, failures 0, msgs/s 392178, p50 65024 ns, p99 143360 ns, p999 315392 ns, max 34203668 ns |
| 24 hour soak | Not run. Command: `DURATION=86400 PORT=19521 bash scripts/soak.sh` |
| Dockerfile, compose.yaml | Implemented but partially verified. `docker.io` is installed. The daemon did not start (`policy-rc.d`). Compose was not executed |
| GitHub Actions workflow | Implemented but partially verified. gcc job runs `scripts/test_compare_benchmarks.sh`. Benchmark and clang-tidy jobs are `workflow_dispatch` and do not fail the pull request on latency or tidy findings. Actions were not re-run in this session |
| CrashLab | Schema matched. HTTP adapter `fluxnet_crashlab_http` tested by `fluxnet_adapter_test`. The CrashLab CLI was not executed because Docker did not start |
| libFuzzer | Implemented and verified for 200 decoder inputs (`scripts/fuzz_decode.sh`, exit 0). Not a multi-hour fuzz |
| Valgrind on the six unit binaries | Implemented and verified. 0 errors from `--leak-check=full`. Integration suite not run under Valgrind |
| perf counters | Blocked. `perf` not installed; `linux-tools-6.12.113` not in apt; `perf_event_paranoid` is 2 |
| 1M small messages/s | Not reached. Highest short active run in this pass is 439820 msgs/s at 10000 connections. The 600 second run was 401951 msgs/s |

## Commands that passed

Debug (`/workspace/build`, g++ 13.3.0) after the completion pass: integration, load-generator, and adapter tests passed. ASan and TSan `ctest -LE stress` passed (10 tests, including the adapter). UBSan `ctest -LE stress` passed and `./build-ubsan/tests/fluxnet_stress` passed. TSan uses `tests/tsan.supp` for liburing ring reuse only.

```bash
ctest --test-dir build --output-on-failure
ctest --test-dir build-asan --output-on-failure -LE stress
ctest --test-dir build-ubsan --output-on-failure -LE stress
./build-ubsan/tests/fluxnet_stress
ctest --test-dir build-tsan --output-on-failure -LE stress
RUNS=200 bash scripts/fuzz_decode.sh
```

10,000 active connections:

```bash
./build-release/examples/fluxnet_echo --port 19401 --workers 1 --epoll --max-connections 20000
./build-release/benchmarks/fluxnet_bench --host 127.0.0.1 --port 19401 --warmup 0.5 --duration 2 \
  --connections 10000 --payload 64 --threads 4 --outstanding 1 \
  --out results/raw/epoll-1w-c10000-p64.json
```

That run finished with failures 0 and connect_failures 0. Charts were regenerated with `python3 scripts/plot_results.py`.

## Specification coverage

| Phase | Status |
| --- | --- |
| 0 Build and environment | Implemented and verified |
| 1 Sockets and blocking baseline | Implemented and verified |
| 2 epoll LT and ET | Implemented and verified |
| 3 Framing and in-process fuzz | Implemented and verified. libFuzzer: 200 inputs, exit 0 |
| 4 Multithreaded reactor and SPSC | Implemented and verified |
| 5 Buffer pool and gather | Implemented and verified. One pooled and one disabled sample. No throughput win |
| 6 Backpressure and timers | Implemented and verified |
| 7 Shutdown | Implemented and verified, including stop while connects are in flight |
| 8 Public API and echo | Implemented and verified |
| 9 io_uring | Implemented and verified for worker recv/send/cancel and acceptor accept, including partial send, close-during-send, tiny ring, and the multishot-recv probe staying off (`-22`). Registered buffers are probed and not used on the data path |
| 10 CPU affinity and perf | Affinity implemented, default off, no measured benefit. perf blocked |
| 11 Echo, pub/sub, KV | Implemented and verified |
| 12 Load generator | Implemented and verified: CSV, churn, slow-read, open loop `behind_schedule`, payload mismatch fails the process, `timed_out` on an unanswered send |
| 13 Comparison matrix | Implemented and verified for the short matrix in BENCHMARKS.md, including 5000 and 10000 active connections. Not a full cartesian product. 8 workers not run on a 4-CPU guest |
| 14 CrashLab | Partially verified. Adapter and schema-valid YAML exist and the adapter test passed. The CrashLab CLI was not run. Blocked by the Docker daemon and `tc` EPERM |
| 15 Tests and sanitizers | Implemented and verified for unit, integration, concurrency, stress, ASan, UBSan, TSan, Valgrind unit binaries, and a 200-input libFuzzer run |
| 16 CI | Implemented but partially verified. Workflow contains gcc, clang, sanitizers, format, compare script, optional benchmark artifact upload, and optional clang-tidy. GitHub did not run this revision here |
| 17 Docs | Implemented and verified against `results/raw` for the runs that finished. No LICENSE file |
| 18 Audit | Implemented and verified for the defects fixed earlier and the liburing TSan note. Not a release recommendation |

## Defects fixed in this pass

`try_send` no longer returns `Accepted` for a frame that is not queued. Histograms are merged before percentiles. `Connection::id()` works during `on_disconnect`. The load generator now exits non-zero when `failures` is non-zero, and its measurement window starts after connects. See `AUDIT.md`.
