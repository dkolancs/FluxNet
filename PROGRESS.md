# Progress

## Authorship

Repo-local `user.name` is `Divith Reddy Kolan` and `user.email` is `dkolan@asu.edu`. New commits on `cursor/fluxnet-correctness-d096` use that identity and do not carry a Co-authored-by trailer. Nothing was pushed. The initial commit still has a historical Co-authored-by line and was not rewritten.

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
| io_uring worker recv/send/cancel, runtime fallback | Implemented and verified (`Echo.IoUringIfAvailable` did not skip; 10-conn bench ran). Async accept is not implemented; the acceptor stays on epoll |
| CPU affinity flag, default off | Implemented and verified as "no benefit on one 10-connection run". Flag stays off. No perf counters |
| Pub/sub and KV examples | Implemented and verified. Disconnect removes the subscription. TSan passed |
| Load generator, merged histogram, JSON | Implemented and verified. Old unmerged files are in `results/legacy-unmerged/` |
| Blocking and thread-per-connection baselines | Implemented and verified (2 s runs after the histogram fix) |
| Boost.Asio baseline | Implemented and verified as an external binary. One 10-connection run. Not linked into libfluxnet |
| 10000 idle connections | Implemented and verified. `connected=10000 failed=0`, RSS 177052 kB. Not an active-message test |
| 600 second soak | Implemented and verified. Wall clock of the script 601 s. `duration_s` 600. Not 1 hour or 24 hours |
| 1 hour and 24 hour soaks | Not run. `scripts/soak.sh` accepts `DURATION` |
| Dockerfile, compose.yaml | Implemented but partially verified (files exist; `docker` is not installed) |
| GitHub Actions workflow | Implemented but partially verified (yaml matches the commands; Actions did not run here) |
| CrashLab | Blocked by environment. No CrashLab repo. `crashlab/INTEGRATION.md` is only a boundary |
| libFuzzer, Valgrind, perf counters | Blocked by environment |
| 1M small messages/s | Not reached. Highest FluxNet figure in `results/raw/` is 401951 msgs/s over 600 seconds |

## Commands that passed

Debug (`/workspace/build`, g++ 13.3.0), 38 GoogleTest cases, 9 ctest binaries:

```bash
ctest --test-dir build --output-on-failure
# 9/9, integration about 14.6 s
```

ASan `build-asan`: 9/9. UBSan `build-ubsan`: 9/9. TSan `build-tsan`: 9/9. Benchmarks were off in the sanitizer configures. No suppressions were added.

Release:

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++ -DFLUXNET_ENABLE_ASIO_BASELINE=ON
DURATION=2 WARMUP=0.5 bash scripts/benchmark.sh
DURATION=600 PORT=19220 bash scripts/soak.sh
CONNECTIONS=10000 bash scripts/scale_connections.sh
python3 scripts/plot_results.py
```

Numbers are only in `BENCHMARKS.md` and `results/raw/`. The chart is `results/plots/throughput.png`.

## Defects fixed in this pass

`try_send` no longer returns `Accepted` for a frame that is not queued. Histograms are merged before percentiles. `Connection::id()` works during `on_disconnect`. See `AUDIT.md`.
