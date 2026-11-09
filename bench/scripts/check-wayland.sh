#!/usr/bin/env bash
# Проверка, что композитор wolf'а вообще отдаёт кадры.
#
# Поднимаем waylanddisplaysrc, подключаем к нему wayland-клиент из стенда
# и смотрим, пошли ли кадры. Если здесь пусто — стенд не сможет ничего
# воспроизвести: нечего кодировать.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"

IMAGE="${WOLF_IMAGE:-ghcr.io/games-on-whales/wolf:latest}"
RUN_DIR="${RUN_DIR:-/tmp/wolf-wayland-check}"
WIDTH="${WIDTH:-640}"
HEIGHT="${HEIGHT:-480}"

rm -rf "$RUN_DIR"
mkdir -p "$RUN_DIR"
chmod 700 "$RUN_DIR"

podman run --rm \
  --device /dev/dri \
  -e XDG_RUNTIME_DIR=/run/user/wolf \
  -v "$RUN_DIR:/run/user/wolf:Z" \
  -v "$BENCH_DIR/wayland-client:/wl:Z" \
  --entrypoint /bin/sh "$IMAGE" -c '
    gst-launch-1.0 waylanddisplaysrc render_node=/dev/dri/renderD128 ! \
      videoconvert ! fpsdisplaysink video-sink=fakesink sync=false > /tmp/fps.log 2>&1 &
    sleep 4
    sock=$(ls /run/user/wolf | grep ^wayland | grep -v lock | head -1)
    echo "socket: $sock"
    WL_SHM_DEBUG=1 WAYLAND_DISPLAY=$sock timeout 6 /wl/build/wl-shm-client '"$WIDTH"' '"$HEIGHT"' 2>&1 | head -8
    echo "--- кадры композитора:"
    tail -5 /tmp/fps.log
  '
