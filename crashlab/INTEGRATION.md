# CrashLab

The real project is `github.com/dkolancs/CrashLab` (public, seen via `gh search repos crashlab --owner dkolancs` and `gh repo list`). It was not on disk under `/workspace`, `/home`, `/opt`, or `/usr/local`. The schema below is from `internal/experiment/file.go` in that repository, not invented here.

An experiment file has `name`, `target.service`, `fault.type` (`latency`, `loss`, `kill`, or `cpu`), optional `fault.delay` / `fault.percent`, `experiment.baseline` / `duration` / `recovery`, `entry.url` (HTTP), `entry.service`, `compose`, and `expect` assertions (`error_rate`, `p95_latency`, `recovery_time`, optional `mode: pass|fail`).

`crashlab/experiments/echo-latency.yaml` uses those fields and points at this repo's `compose.yaml` service `echo`. It was not executed.

Why it was not run:

- `docker info` failed: `unix:///var/run/docker.sock` does not exist. `apt` installed `docker.io`, then `invoke-rc.d` reported `policy-rc.d denied execution of start`, so the daemon never came up.
- CrashLab's entry probe is an HTTP URL. `fluxnet_echo` speaks the 8-byte TCP frame protocol, not HTTP, so a successful `crashlab experiment run` would not be measuring this library's messages even if Docker were up.
- `tc qdisc add dev lo root netem delay 1ms` returned `RTNETLINK answers: Operation not permitted`.

`experiments/faults/cpu_stress.sh` is a separate local script. It is not a CrashLab command. It ran `stress-ng --cpu 2` around three 2-second echo benchmarks. The JSON files are `results/raw/fault-cpu-before.json`, `fault-cpu-during.json`, and `fault-cpu-after.json`.

Application `Server::stop()` drains until `shutdown_timeout` and then `run()` returns. That is not a process restart and not a client reconnect. The load generator reconnects only when `--churn-seconds` is set. A dropped connection otherwise stays a connect or message failure.
