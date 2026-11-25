# Progress

## Authorship — do not commit

```
git config --get user.name   -> Cursor Agent
git config --get user.email  -> cursoragent@cursor.com
git shortlog -sne --all      -> 1  Cursor Agent <cursoragent@cursor.com>
git log --all --format="%an <%ae> | %cn <%ce>"
  Cursor Agent <cursoragent@cursor.com> | Cursor Agent <cursoragent@cursor.com>
```

The only commit message is `Initial commit` plus `Co-authored-by: dkolancs <dkolancs@users.noreply.github.com>`. That trailer was already in history. It was not added or rewritten by this session.

The configured identity is an agent identity. **No commit was made. Nothing was pushed.** The library is untracked in the working tree (`git status` shows untracked files under `include/`, `src/`, `tests/`, and the docs).

## What is implemented and verified

| Feature | Status |
| --- | --- |
| C++20 CMake, Ninja, Debug/Release, ASan, UBSan, TSan | Implemented and verified |
| RAII `FileDescriptor`, TCP socket helpers | Implemented and verified |
| 8-byte frame codec, incremental decode, in-process fuzz | Implemented and verified |
| Epoll reactor, LT and ET, generation tokens, eventfd wake | Implemented and verified |
| Acceptor + N workers, round-robin and least-loaded, admission cap | Implemented and verified (round-robin and the cap have tests; least-loaded is compiled and not separately measured) |
| Mutex queue and lock-free SPSC handoff | Implemented and verified (unit, threaded checksum, uncontended microbench) |
| Worker-local buffer pool and `sendmsg` gather | Implemented and verified (pool unit tests; gather is the default write path, no separate on/off benchmark) |
| Watermarks, idle timeout, slow-consumer timeout, timerfd | Implemented and verified (idle test; slow-reader test checks a byte bound, not a timeout close) |
| signalfd shutdown, lifecycle states | Implemented and verified by tests that `stop()` and join |
| Public server/client, echo example | Implemented and verified |
| io_uring worker recv/send, runtime fallback | Implemented and verified (echo integration did not skip; 10-conn bench ran) |
| CPU affinity flag, default off | Implemented but partially verified (code honors `sched_getaffinity`; no before/after benchmark, flag was off) |
| Pub/sub and KV examples | Implemented and verified (in-process tests) |
| Load generator, JSON, histogram, compare/plot scripts | Implemented and verified for the runs in `BENCHMARKS.md` |
| Blocking and thread-per-connection baselines | Implemented and verified (2 s runs) |
| 45 s smoke soak | Implemented and verified as 45 s only |
| Dockerfile, compose.yaml | Implemented but partially verified (files exist; `docker` is not installed, so they were not run) |
| GitHub Actions workflow | Implemented but partially verified (the yaml matches the commands used here; Actions did not run in this session) |
| CrashLab experiments | Not implemented. Blocked by environment: no CrashLab repo. See `crashlab/INTEGRATION.md` |
| Boost.Asio baseline | Not implemented. Boost headers were not installed |
| libFuzzer, Valgrind, perf counters, flame graph | Blocked by environment |
| 10 minute, 1 hour, 24 hour soaks | Not implemented as executed runs. `scripts/soak.sh` takes `DURATION`. Only 45 s was executed |
| 10k connections, 1M msgs/s | Not reached. Highest measured short-run throughput is 373133 msgs/s; 45 s run was 419925 msgs/s |

## Commands that passed

Debug (`/workspace/build`, g++ 13.3.0):

```bash
ctest --test-dir build --output-on-failure
# 9/9 passed, integration 16.49 s
# 33 gtest cases: fd 2, socket 4, codec 5, queue 3, pool 3, histogram 2,
# integration 10, concurrency 2, fuzz 2
```

ASan `build-asan`: 9/9. UBSan `build-ubsan`: 9/9. TSan `build-tsan`: 9/9 after the broker lifetime fix in `tests/integration/test_apps.cpp`.

Release benchmarks: `DURATION=2 WARMUP=0.5 bash scripts/benchmark.sh` plus the 1000-connection sample, one repeat, the queue microbench, and `DURATION=45 bash scripts/soak.sh`. Numbers are only in `BENCHMARKS.md` and `results/raw/`.

After `clang-format`, debug `fluxnet_integration` was rebuilt and passed 10/10. Sanitizer ctests were not re-run after formatting.

## Defects fixed

See `AUDIT.md`. The two that hung or raced startup were the condition-variable notify and `wait_until_running` treating the initial `Stopped` as done.

## Next step if another session resumes

The git identity is still the blocker for commits. After a personal identity is configured (do not invent one, do not set it from this file), the first commit can be the tree that passed the commands above. Do not amend `5e0a6aa`.

Useful follow-ups that are not done: a merged histogram in the load generator, an on/off pool benchmark, least-loaded as its own test, a 10-minute soak if you actually have the time, and a real CrashLab schema if the repo shows up.
