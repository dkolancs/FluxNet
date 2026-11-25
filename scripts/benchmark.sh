#!/usr/bin/env bash
# Short localhost matrix. Not a full cartesian product.
# Override DURATION and WARMUP (seconds). Defaults are short on purpose.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build-release}"
OUT="${OUT:-$ROOT/results/raw}"
DURATION="${DURATION:-2}"
WARMUP="${WARMUP:-0.5}"
mkdir -p "$OUT"

if [[ ! -x "$BUILD/examples/fluxnet_echo" || ! -x "$BUILD/benchmarks/fluxnet_bench" ]]; then
  echo "missing release binaries in $BUILD" >&2
  echo "configure with: cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=/usr/bin/g++" >&2
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
  for _ in $(seq 1 50); do
    if (echo >/dev/tcp/127.0.0.1/"$port") >/dev/null 2>&1; then
      ready=1
      break
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "server exited early" >&2
      cat /tmp/fluxnet-server-"$port".log >&2 || true
      exit 1
    fi
    sleep 0.05
  done
  if [[ "$ready" != 1 ]]; then
    echo "server did not accept on $port" >&2
    kill "$pid" 2>/dev/null || true
    exit 1
  fi
  set +e
  "$BUILD/benchmarks/fluxnet_bench" --host 127.0.0.1 --port "$port" \
    --warmup "$WARMUP" --duration "$DURATION" --out "$OUT/$name.json" "$@"
  local rc=$?
  set -e
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  if [[ "$rc" != 0 ]]; then
    echo "bench $name failed rc=$rc" >&2
    exit "$rc"
  fi
}

run_case epoll-1w-c1-p64 \
  "$BUILD/examples/fluxnet_echo --port 19101 --workers 1 --epoll" 19101 \
  --connections 1 --payload 64 --threads 1 --outstanding 1

run_case epoll-1w-c10-p64 \
  "$BUILD/examples/fluxnet_echo --port 19102 --workers 1 --epoll" 19102 \
  --connections 10 --payload 64 --threads 1 --outstanding 1

run_case epoll-1w-c100-p64 \
  "$BUILD/examples/fluxnet_echo --port 19103 --workers 1 --epoll" 19103 \
  --connections 100 --payload 64 --threads 2 --outstanding 1

run_case epoll-4w-c100-p64 \
  "$BUILD/examples/fluxnet_echo --port 19104 --workers 4 --epoll" 19104 \
  --connections 100 --payload 64 --threads 2 --outstanding 1

run_case epoll-4w-c10-p1024 \
  "$BUILD/examples/fluxnet_echo --port 19105 --workers 4 --epoll" 19105 \
  --connections 10 --payload 1024 --threads 1 --outstanding 1

run_case epoll-et-1w-c10-p64 \
  "$BUILD/examples/fluxnet_echo --port 19106 --workers 1 --epoll --edge" 19106 \
  --connections 10 --payload 64 --threads 1 --outstanding 1

run_case uring-1w-c10-p64 \
  "$BUILD/examples/fluxnet_echo --port 19107 --workers 1 --uring" 19107 \
  --connections 10 --payload 64 --threads 1 --outstanding 1

run_case blocking-c1-p64 \
  "$BUILD/benchmarks/fluxnet_blocking_echo --port 19108" 19108 \
  --connections 1 --payload 64 --threads 1 --outstanding 1

run_case threadpc-c10-p64 \
  "$BUILD/benchmarks/fluxnet_thread_echo --port 19109" 19109 \
  --connections 10 --payload 64 --threads 1 --outstanding 1

echo "raw json in $OUT"
