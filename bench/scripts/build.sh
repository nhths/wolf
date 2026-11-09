#!/usr/bin/env bash
# Сборка bench-клиента.
#
# Зависимости (moonlight-common-c с сабмодулями enet/nanors) подтягиваются
# на лету через FetchContent — git submodule update делать не нужно.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"

cmake -S "$BENCH_DIR/client" -B "$BENCH_DIR/client/build" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$BENCH_DIR/client/build" -j"$(nproc 2>/dev/null || echo 4)"

echo "готов: $BENCH_DIR/client/build/bench-client"
