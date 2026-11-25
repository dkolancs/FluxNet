# CrashLab

The real project is `github.com/dkolancs/CrashLab` (public, seen via `gh search repos crashlab --owner dkolancs` and `gh repo list`). It was not on disk under `/workspace`, `/home`, `/opt`, or `/usr/local`. The schema below is from `internal/experiment/file.go` in that repository, not invented here.

An experiment file has `name`, `target.service`, `fault.type` (`latency`, `loss`, `kill`, or `cpu`), optional `fault.delay` / `fault.percent`, `experiment.baseline` / `duration` / `recovery`, `entry.url` (HTTP), `entry.service`, `compose`, and `expect` assertions (`error_rate`, `p95_latency`, `recovery_time`, optional `mode: pass|fail`).

`crashlab/experiments/echo-latency.yaml` uses those fields and points at this repo's `compose.yaml` service `echo`. It was not executed.

Why it was not run on this guest:

- `docker info` failed: `unix:///var/run/docker.sock` does not exist. `apt` installed `docker.io`, then `invoke-rc.d` reported `policy-rc.d denied execution of start`.
- `tc qdisc add dev lo root netem delay 1ms` returned `RTNETLINK answers: Operation not permitted`.

The HTTP mismatch is handled in this repo by `examples/crashlab_http`. `fluxnet_adapter_test` got HTTP 200 from a framed PING without Docker.

`experiments/faults/cpu_stress.sh` is not a CrashLab command. It ran `stress-ng --cpu 2`. Results: `results/raw/fault-cpu-before.json` 201611 msgs/s, `fault-cpu-during.json` 189836 msgs/s, `fault-cpu-after.json` 202305 msgs/s, all with 0 failures.

## How to run it on a host where Docker works

CrashLab is not modified. From a checkout of `github.com/dkolancs/CrashLab` and a checkout of this repo:

```bash
# this repo
docker compose up --build -d
# CrashLab checkout, using this repo's experiment file
go run ./cmd/crashlab experiment run /path/to/FluxNet/crashlab/experiments/echo-latency.yaml
```

`entry.url` is `http://127.0.0.1:8081/`, which is `fluxnet_crashlab_http`. That process opens a TCP connection to the `echo` service, sends one framed PING, and returns HTTP 200 or 502. `target.service` is `echo`, which is the service CrashLab's `tc netem` would delay. The YAML keys are the ones `internal/experiment/file.go` parses.

On this guest `docker info` still fails because `/var/run/docker.sock` does not exist. The adapter test `fluxnet_adapter_test` passed without Docker.

Application `Server::stop()` drains until `shutdown_timeout` and then `run()` returns. That is not a process restart and not a client reconnect. The load generator reconnects only when `--churn-seconds` is set. A dropped connection otherwise stays a connect or message failure.
