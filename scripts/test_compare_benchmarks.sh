#!/usr/bin/env bash
# Fixture checks for compare_benchmarks.py. Not a performance gate.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="$ROOT/scripts/compare_benchmarks.py"
DATA="$ROOT/scripts/testdata"
python3 "$PY" "$DATA/compare_same_a.json" "$DATA/compare_same_a.json"
set +e
python3 "$PY" "$DATA/compare_same_a.json" "$DATA/compare_other_cpu.json"
rc=$?
set -e
if [[ "$rc" != 2 ]]; then
  echo "expected exit 2 for a different cpu, got $rc" >&2
  exit 1
fi
set +e
python3 "$PY" "$DATA/compare_same_a.json" "$DATA/compare_same_b.json" --max-drop 10 --max-p99-increase 25
rc=$?
set -e
if [[ "$rc" != 1 ]]; then
  echo "expected exit 1 for a throughput drop, got $rc" >&2
  exit 1
fi
echo "compare_benchmarks fixtures passed"
