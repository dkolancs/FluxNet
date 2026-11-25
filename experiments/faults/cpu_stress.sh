#!/usr/bin/env bash
# Local CPU contention. This is not a CrashLab experiment and does not use
# CrashLab's schema. tc netem is not permitted in this guest (EPERM).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build-release}"
OUT="${OUT:-$ROOT/results/raw}"
mkdir -p "$OUT"
if [[ ! -x "$BUILD/examples/fluxnet_echo" || ! -x "$BUILD/benchmarks/fluxnet_bench" ]]; then
  echo "missing release binaries" >&2
  exit 1
fi
if ! command -v stress-ng >/dev/null 2>&1; then
  echo "stress-ng is not installed" >&2
  exit 1
fi

run_one() {
  local name="$1"
  local port="$2"
  "$BUILD/examples/fluxnet_echo" --port "$port" --workers 1 --epoll >/tmp/fluxnet-fault-"$port".log 2>&1 &
  local pid=$!
  for _ in $(seq 1 50); do
    if (echo >/dev/tcp/127.0.0.1/"$port") >/dev/null 2>&1; then
      break
    fi
    sleep 0.05
  done
  "$BUILD/benchmarks/fluxnet_bench" --host 127.0.0.1 --port "$port" \
    --warmup 0.2 --duration 2 --connections 10 --payload 64 --threads 1 \
    --outstanding 1 --out "$OUT/$name.json"
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

run_one fault-cpu-before 19301
stress-ng --cpu 2 --timeout 3s >/tmp/fluxnet-stress-ng.log 2>&1 &
stress=$!
sleep 0.3
run_one fault-cpu-during 19302
wait "$stress" || true
run_one fault-cpu-after 19303
echo "wrote fault-cpu-before/during/after json"
