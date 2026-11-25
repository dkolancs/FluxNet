#!/usr/bin/env bash
# Run the echo server and the load generator for DURATION seconds.
# This is only a soak of the length you pass. A 45 second run is not a 10 minute soak.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build-release}"
DURATION="${DURATION:-45}"
PORT="${PORT:-19200}"
mkdir -p "$ROOT/results/raw"
"$BUILD/examples/fluxnet_echo" --port "$PORT" --workers 2 --epoll >/tmp/fluxnet-soak.log 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true' EXIT
for _ in $(seq 1 50); do
  if (echo >/dev/tcp/127.0.0.1/"$PORT") >/dev/null 2>&1; then
    break
  fi
  sleep 0.05
done
"$BUILD/benchmarks/fluxnet_bench" --host 127.0.0.1 --port "$PORT" \
  --connections 32 --payload 64 --threads 2 --outstanding 1 \
  --warmup 1 --duration "$DURATION" \
  --out "$ROOT/results/raw/soak-${DURATION}s.json"
echo "finished soak duration=${DURATION}s (this is not a longer soak)"
