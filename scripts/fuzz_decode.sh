#!/usr/bin/env bash
# Bounded libFuzzer run of the frame decoder.
# Requires clang++ and libclang-rt-18-dev. This guest's clang does not search
# g++ headers or libstdc++ unless those paths are set.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export LIBRARY_PATH="/usr/lib/gcc/x86_64-linux-gnu/13:${LIBRARY_PATH:-}"
clang++ -std=c++20 -stdlib=libstdc++ -fsanitize=fuzzer,address \
  -I/usr/include/c++/13 -I/usr/include/x86_64-linux-gnu/c++/13 \
  -I"$ROOT/include" -I"$ROOT/src" \
  "$ROOT/tests/fuzz/decode_fuzzer.cpp" \
  "$ROOT/src/protocol/codec.cpp" \
  "$ROOT/src/core/config.cpp" \
  -o /tmp/fluxnet_decode_fuzzer
/tmp/fluxnet_decode_fuzzer -runs="${RUNS:-200}" -max_total_time="${MAX_SECONDS:-20}"
