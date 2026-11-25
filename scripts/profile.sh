#!/usr/bin/env bash
# Record perf stat for one short command, or the exact reason it cannot run.
# Does not invent counters. Does not change the default affinity policy.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$ROOT/results/raw/perf.txt}"
mkdir -p "$(dirname "$OUT")"
PARANOID="$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
{
  echo "perf_event_paranoid=$PARANOID"
  if ! command -v perf >/dev/null 2>&1; then
    echo "perf is not installed. No counters collected."
    exit 0
  fi
  if [[ "$PARANOID" != "unknown" && "$PARANOID" -gt 1 ]]; then
    echo "perf_event_paranoid blocks hardware counters for this user. Not sampling."
    exit 0
  fi
  if [[ "${1:-}" != "--" ]]; then
    echo "perf is installed and permitted. Re-run with: profile.sh -- <command>"
    exit 0
  fi
  shift
  echo "command: $*"
  perf stat "$@"
} >"$OUT" 2>&1
echo "wrote $OUT"
