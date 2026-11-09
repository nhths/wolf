#!/usr/bin/env bash
# Сборка wayland-клиента для стенда.
#
# Клиент должен видеть ту же libwayland, что и композитор wolf'а, поэтому
# собираем его внутри образа wolf: там есть и gcc, и wayland-scanner,
# и libwayland-client.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"

IMAGE="${WOLF_IMAGE:-ghcr.io/games-on-whales/wolf:sha-db709cf}"
SRC_DIR="$BENCH_DIR/wayland-client"

mkdir -p "$SRC_DIR/build"

podman run --rm \
  -v "$SRC_DIR:/src:Z" \
  --entrypoint /bin/sh "$IMAGE" -c '
    set -e
    cd /src
    wayland-scanner client-header /usr/share/wayland/wayland.xml wayland-client-protocol.h
    wayland-scanner private-code /usr/share/wayland/wayland.xml wayland-protocol.c
    wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml xdg-shell-client-protocol.h
    wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml xdg-shell-protocol.c
    gcc -O2 -Wall -o build/wl-shm-client wl-shm-client.c wayland-protocol.c xdg-shell-protocol.c -lwayland-client
  '

echo "собрано: $SRC_DIR/build/wl-shm-client"
