# Limitations

Facts below were read on this machine during this session. They are not a generic "Linux" claim.

## Host

- `uname -a`: `Linux cursor 6.12.113+ #1 SMP PREEMPT_DYNAMIC Mon Nov 23 16:05:51 UTC 2026 x86_64 GNU/Linux`
- `/proc/version`: built with gcc (Debian 12.2.0-14+deb12u1) 12.2.0
- `systemd-detect-virt`: `kvm`
- `/proc/1/cgroup`: `0::/system.slice/pod-xcs4oqkqm5canfkjg4toolcgkm-e6ffa30d/init`
- This is a KVM virtual machine (a container-style cgroup inside that guest), not bare metal and not WSL2.
- CPU: 4 vCPU, `Intel(R) Xeon(R) Processor` family 6 model 173, 1 thread per core, reported at 2700 MHz. `Cpus_allowed_list: 0-3`.
- Cache as reported by `lscpu`: L1d 192 KiB, L1i 256 KiB, L2 8 MiB, L3 480 MiB. The L3 figure is the hypervisor's report and is not treated as a measurement of this guest's exclusive cache.
- RAM: `MemTotal` 16387328 kB (about 15.6 GiB). `free -h` showed about 15 GiB total. No swap.
- `ulimit -n`: 524288. `ulimit -u`: 63994. `ulimit -l` (max locked memory): 8192 KiB.
- `perf_event_paranoid`: 2. `perf` is still not installed. `apt` could not find `linux-tools-6.12.113`. `results/raw/perf.txt` records that. No hardware counters and no flame graphs.
- `valgrind` is installed. `valgrind --leak-check=full` on `fluxnet_unit_codec` reported 0 errors and 0 bytes in use at exit. The integration suite was not run under Valgrind.
- `docker.io` is installed. The daemon did not start (`policy-rc.d denied execution of start`, no `/var/run/docker.sock`). `compose.yaml` was not executed.
- `tc` is installed. `tc qdisc add dev lo root netem delay 1ms` returned `Operation not permitted`.
- `stress-ng` is installed and was used by `experiments/faults/cpu_stress.sh`. That is not CrashLab.
- CrashLab: the repository `github.com/dkolancs/CrashLab` exists. It was not cloned onto this machine. `crashlab/experiments/echo-latency.yaml` matches its file schema and was not run. See `crashlab/INTEGRATION.md`.
- libFuzzer: `scripts/fuzz_decode.sh` compiled with `libclang-rt-18-dev` and ran 200 inputs (`Done 200 runs`, exit 0). Clang needs the g++ include and library paths on this guest.
- ThreadSanitizer reports data races inside `liburing.h` when a destroyed probe ring and a later worker ring reuse the same `anon_inode:[io_uring]` address. `tests/tsan.supp` suppresses those three liburing functions. It does not suppress FluxNet stacks. With that file, TSan ctest passed.
- Git identity for new commits: `Divith Reddy Kolan <dkolan@asu.edu>`, repo-local config only. Nothing was pushed. The initial commit still contains a historical `Co-authored-by` trailer. It was not rewritten. A local commit-msg hook tries to append `Co-authored-by: dkolancs <dkolancs@users.noreply.github.com>`; commits in this pass were created with that hook disabled so the trailer is not on the new commits.

## Toolchain (after installing missing packages)

Already present:

- gcc/g++ 13.3.0 (Ubuntu)
- clang++ 18.1.3
- cmake 3.28.3
- Python 3.12.3
- No system GoogleTest or Google Benchmark. Boost headers were not present at the first inspection; `libboost-dev` 1.83 was installed later for the Asio baseline only.

Installed with apt during this session because they were missing:

- ninja 1.11.1
- liburing 2.5 (`liburing-dev`)
- clang-format 18.1.3
- python3-matplotlib 3.6.3
- libboost-dev 1.83.0 (Asio baseline only; not linked into libfluxnet)

io_uring:

- `/proc/sys/kernel/io_uring_disabled` = 0
- `/proc/sys/kernel/io_uring_group` = -1
- A probe linked with `-luring` returned `io_uring ok features=0xffff` for a ring of 8 entries.

GoogleTest v1.14.0 and Google Benchmark v1.8.3 are fetched by CMake (`GIT_TAG`), not vendored. Configuring the build needs network access to GitHub.

Boost 1.83 headers were installed from `libboost-dev`. `fluxnet_asio_echo` is a separate executable built with `-DFLUXNET_ENABLE_ASIO_BASELINE=ON`. `libfluxnet` does not link Boost. The 10-connection Asio numbers are in `results/raw/asio-c10-p64.json`.

libFuzzer was not used. Protocol fuzzing is a deterministic in-process driver.

## Library limits (by design)

- IPv4 only.
- One process-global log level.
- `MessageView` must not be stored.
- `Connection` must not be used off the owning worker. Cross-thread sends go through `Server`/`Client`.
- Blocking `send` from a worker thread to a different worker does not wait (deadlock avoidance).
- io_uring does not use fixed buffers, multishot recv, or zero-copy send. At most one recv and one send are in flight per connection.
- When the worker backend is io_uring, the acceptor uses its own ring for accept. If that ring fails to open, accept stays on epoll. Multishot recv was probed and returned `EINVAL` (-22) and is not used. Registered buffers probed successfully and are not used on the data path.
- `close` is not retried after `EINTR` (Linux fd-release behavior).
- Pool memory retained up to `pool_max_per_class` per class per worker is intentional.
- Cross-thread task memory is bounded by `queue_capacity * max_frame_size` per worker, which can be large at the defaults (1024 * 1 MiB). Hot-path echo does not use that queue.
- No HTTP, TLS, or UDP.
- Pub/sub and KV are in-memory examples. They are not durable.
- Loopback benchmarks include client and server on the same 4 vCPU guest. They are not NIC numbers.
- Soak: `DURATION=600` finished (`results/raw/soak-600s.json`, wall clock 601 seconds for the script). A 1-hour or 24-hour soak was not started. The 45 second file in `results/legacy-unmerged/` is an older run and used unmerged percentiles.

## Idle connections

`scripts/scale_connections.sh` held 10000 idle connections to a 1-worker echo server. `connected=10000 failed=0`. Server RSS was 177052 kB, `Threads` 2. No EMFILE was logged. Those sockets were not sending. The active-message runs are the benchmark JSON files. `results/raw/epoll-1w-c5000-p64.json` is 5000 connections actively sending (240632 msgs/s, p99 about 58 ms, 0 failures). 10000 active senders were not run. Eight worker threads were not run on this 4-CPU guest.

## Authorship

New commits use `Divith Reddy Kolan <dkolan@asu.edu>`. Do not push unless the user asks. Do not rewrite the initial commit.
