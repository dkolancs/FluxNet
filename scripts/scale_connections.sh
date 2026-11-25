#!/usr/bin/env bash
# Hold CONNECTIONS idle TCP sessions against fluxnet_echo and record server RSS.
# This is not CrashLab. It does not claim a connection count the process did not hold.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build-release}"
PORT="${PORT:-19300}"
CONNECTIONS="${CONNECTIONS:-10000}"
HOLD="${HOLD:-3}"
OUT="${OUT:-$ROOT/results/raw/scale-${CONNECTIONS}.txt}"
mkdir -p "$(dirname "$OUT")"

if [[ ! -x "$BUILD/examples/fluxnet_echo" || ! -x "$BUILD/benchmarks/fluxnet_hold" ]]; then
  echo "missing binaries in $BUILD" >&2
  exit 1
fi

"$BUILD/examples/fluxnet_echo" --port "$PORT" --workers 1 --epoll >/tmp/fluxnet-scale.log 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true' EXIT
for _ in $(seq 1 50); do
  if (echo >/dev/tcp/127.0.0.1/"$PORT") >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "echo exited" >&2
    cat /tmp/fluxnet-scale.log >&2 || true
    exit 1
  fi
  sleep 0.05
done

set +e
"$BUILD/benchmarks/fluxnet_hold" --host 127.0.0.1 --port "$PORT" \
  --connections "$CONNECTIONS" --hold "$HOLD" >/tmp/fluxnet-hold.out 2>&1 &
hold_pid=$!
for _ in $(seq 1 600); do
  if grep -q '^connected=' /tmp/fluxnet-hold.out 2>/dev/null; then
    break
  fi
  if ! kill -0 "$hold_pid" 2>/dev/null; then
    break
  fi
  sleep 0.1
done
# The holder prints the count before sleeping, so this sample is during the hold.
sleep 0.3
rss="unknown"
hwm="unknown"
threads="unknown"
if [[ -r /proc/$pid/status ]]; then
  rss=$(awk '/VmRSS:/ {print $2}' /proc/"$pid"/status)
  hwm=$(awk '/VmHWM:/ {print $2}' /proc/"$pid"/status)
  threads=$(awk '/Threads:/ {print $2}' /proc/"$pid"/status)
fi
wait "$hold_pid"
rc=$?
set -e
{
  echo "connections_requested=$CONNECTIONS"
  cat /tmp/fluxnet-hold.out
  echo "server_pid=$pid"
  echo "server_rss_kb=$rss"
  echo "server_hwm_kb=$hwm"
  echo "server_threads=$threads"
  echo "hold_exit=$rc"
  echo "note=idle connections only; client sockets are in fluxnet_hold, RSS is the echo process"
} | tee "$OUT"
exit "$rc"
