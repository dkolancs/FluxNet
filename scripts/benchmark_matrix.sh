#!/usr/bin/env bash
# One-variable localhost cells that the short benchmark.sh matrix does not cover.
# Duration defaults to 2s. Runs one server and one generator at a time.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build-release}"
OUT="${OUT:-$ROOT/results/raw}"
DURATION="${DURATION:-2}"
WARMUP="${WARMUP:-0.5}"
mkdir -p "$OUT"

if [[ ! -x "$BUILD/examples/fluxnet_echo" || ! -x "$BUILD/benchmarks/fluxnet_bench" ]]; then
  echo "missing release binaries in $BUILD" >&2
  exit 1
fi

run_case() {
  local name="$1"
  local server_cmd="$2"
  local port="$3"
  shift 3
  echo "== $name =="
  # shellcheck disable=SC2086
  $server_cmd >/tmp/fluxnet-server-"$port".log 2>&1 &
  local pid=$!
  local ready=0
  for _ in $(seq 1 100); do
    if (echo >/dev/tcp/127.0.0.1/"$port") >/dev/null 2>&1; then
      ready=1
      break
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "server exited early" >&2
      cat /tmp/fluxnet-server-"$port".log >&2 || true
      return 1
    fi
    sleep 0.05
  done
  if [[ "$ready" != 1 ]]; then
    echo "server did not accept on $port" >&2
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    return 1
  fi
  set +e
  "$BUILD/benchmarks/fluxnet_bench" --host 127.0.0.1 --port "$port" \
    --warmup "$WARMUP" --duration "$DURATION" --out "$OUT/$name.json" "$@"
  local rc=$?
  set -e
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  return "$rc"
}

run_case epoll-1w-c10-p256 \
  "$BUILD/examples/fluxnet_echo --port 19201 --workers 1 --epoll" 19201 \
  --connections 10 --payload 256 --threads 1 --outstanding 1

run_case epoll-1w-c10-p4096 \
  "$BUILD/examples/fluxnet_echo --port 19202 --workers 1 --epoll" 19202 \
  --connections 10 --payload 4096 --threads 1 --outstanding 1

run_case epoll-1w-c10-p65536 \
  "$BUILD/examples/fluxnet_echo --port 19203 --workers 1 --epoll" 19203 \
  --connections 10 --payload 65536 --threads 1 --outstanding 1

run_case epoll-2w-c100-p64 \
  "$BUILD/examples/fluxnet_echo --port 19204 --workers 2 --epoll" 19204 \
  --connections 100 --payload 64 --threads 2 --outstanding 1

CPUS="$(nproc)"
if [[ "$CPUS" -ge 8 ]]; then
  run_case epoll-8w-c100-p64 \
    "$BUILD/examples/fluxnet_echo --port 19205 --workers 8 --epoll" 19205 \
    --connections 100 --payload 64 --threads 2 --outstanding 1
else
  echo "worker count 8 is not applicable on this ${CPUS}-CPU guest; not oversubscribing" | tee "$OUT/workers-8-not-applicable.txt"
fi

for rep in 1 2 3; do
  run_case "epoll-1w-c10-p64-r${rep}" \
    "$BUILD/examples/fluxnet_echo --port 19206 --workers 1 --epoll" 19206 \
    --connections 10 --payload 64 --threads 1 --outstanding 1
done

run_case epoll-1w-c10-p64-openloop \
  "$BUILD/examples/fluxnet_echo --port 19207 --workers 1 --epoll" 19207 \
  --connections 10 --payload 64 --threads 1 --outstanding 1 --rate 50000

run_case epoll-1w-c4-p64-churn \
  "$BUILD/examples/fluxnet_echo --port 19208 --workers 1 --epoll" 19208 \
  --connections 4 --payload 64 --threads 1 --outstanding 1 --churn-seconds 0.5

set +e
run_case epoll-1w-c5000-p64 \
  "$BUILD/examples/fluxnet_echo --port 19209 --workers 1 --epoll" 19209 \
  --connections 5000 --payload 64 --threads 4 --outstanding 1
active_rc=$?
set -e
if [[ "$active_rc" != 0 ]]; then
  echo "active 5000-connection run failed rc=$active_rc" | tee "$OUT/active-5000-failed.txt"
  echo "server log:" >> "$OUT/active-5000-failed.txt"
  cat /tmp/fluxnet-server-19209.log >> "$OUT/active-5000-failed.txt" || true
fi

echo "matrix json in $OUT"
