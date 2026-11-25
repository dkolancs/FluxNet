#!/usr/bin/env bash
# perf is optional. This script records the blocker instead of inventing counters.
set -euo pipefail
if ! command -v perf >/dev/null 2>&1; then
  echo "perf is not installed. No counters collected."
  exit 0
fi
PARANOID="$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
echo "perf_event_paranoid=$PARANOID"
if [[ "$PARANOID" != "unknown" && "$PARANOID" -gt 1 ]]; then
  echo "perf_event_paranoid blocks hardware counters for this user. Not sampling."
  exit 0
fi
echo "perf is usable. Pass a command after -- to record it."
if [[ "${1:-}" == "--" ]]; then
  shift
  perf stat "$@"
fi
