# CrashLab boundary

No CrashLab repository, CLI, schema, or config was found on this machine. Searches covered `/workspace`, `/home`, `/opt`, and `/usr/local`.

Because the real schema is absent, this directory does not invent experiment fields, fault names, or report layouts. Doing that would look like an integration and would not be one.

What FluxNet can hand to a future harness, once that harness's real interface is known:

- Process under test: `fluxnet_echo`, `fluxnet_pubsub`, or `fluxnet_kv`, bound to `127.0.0.1`.
- Load: `fluxnet_bench`, which writes JSON with completed messages, failures, and latency percentiles.
- Lifecycle the library actually implements: `stop()` drains until `shutdown_timeout`, then `run()` returns. That is application shutdown, not a process restart, and not a client reconnect. The load generator does not reconnect after a failure in the current version.
- Metrics the library actually keeps: `MetricsSnapshot` in `include/fluxnet/metrics.hpp`.

`docker` and `tc` were not installed here, so no fault injection was executed. `compose.yaml` at the repo root only starts the echo server and the load generator. It is not a CrashLab experiment.

Status: **Not implemented** as a CrashLab integration. **Blocked by environment** for running one, because the CrashLab repo is not present.
