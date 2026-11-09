#!/usr/bin/env bash
# Локальный стенд: wolf поднимается в podman на этой же машине.
#
#   ./scripts/run-local.sh build   # собрать wayland-клиент и bench-клиент
#   ./scripts/run-local.sh up      # поднять wolf, дождаться API
#   ./scripts/run-local.sh run     # два клиента (+ лобби по LOBBY=1)
#   ./scripts/run-local.sh down    # остановить и удалить контейнер
#   ./scripts/run-local.sh all     # build + up + run + down
#
# Режим NET=host (по умолчанию) кладёт wolf в сетевой namespace хоста:
# тогда RTP идёт через loopback, и на нём можно включить netem — то есть
# получить «VPN-линк» с потерями без второй машины.
#
# Ничего из этого не ходит в сеть: wolf, клиенты и перехват трафика
# живут на loopback этой машины.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"

IMAGE="${WOLF_IMAGE:-cr.knois.ru/mirror-github/games-on-whales/wolf:sha-db709cf}"
CONTAINER="${CONTAINER:-wolf-bench}"
WORK_DIR="${WORK_DIR:-/tmp/wolf-bench-local}"
NET="${NET:-host}"
HTTP_PORT="${HTTP_PORT:-47989}"
HTTPS_PORT="${HTTPS_PORT:-47984}"
VIDEO_PORT="${VIDEO_PORT:-47999}"
# RTP-ping порты wolf'а (WOLF_VIDEO_PING_PORT/WOLF_AUDIO_PING_PORT): клиент
# шлёт по ним ping, и только после этого wolf запускает видео-конвейер.
# Стандартные 48100/48200 на этой машине заняты NetworkManager'ом.
PING_VIDEO_PORT="${PING_VIDEO_PORT:-48110}"
PING_AUDIO_PORT="${PING_AUDIO_PORT:-48210}"

WIDTH="${WIDTH:-1280}"
HEIGHT="${HEIGHT:-720}"
FPS="${FPS:-60}"
BITRATE="${BITRATE:-20000}"
DURATION="${DURATION:-60}"
LOBBY="${LOBBY:-1}"
PIN="${PIN:-1234}"
# Адрес, с которого стенд ходит на wolf. Важен: moonlight-common-c
# считает адрес «локальным», если он приватный (10/8, 172.16/12,
# 192.168/16, 169.254/16) — и только тогда НЕ каптит размер RTP-пакета
# до 1024. Loopback (127/8) в список не входит, поэтому через 127.0.0.1
# клиент всегда считает сервер удалённым. Чтобы повторить поведение
# VPN-клиента (адрес 172.26.0.x), стенд ходит на адрес машины в LAN.
HOST="${HOST:-127.0.0.1}"
# потери на loopback (процент пакетов) — эмуляция «VPN-линка»
LOSS="${LOSS:-0}"
# NOISE=1 — приложение рисует шум вместо градиента: кодировщик упирается
# в CPU, конвейер отстаёт и роняет кадры (иначе симптом не воспроизводится)
NOISE="${NOISE:-0}"

# размер RTP-пакета, который просит клиент. 1392 — дефолт Moonlight
# (CAP_NET_UDP, LAN); 1024 — то, что клиент выбирает для «удалённого»
# IPv4-адреса. wolf берёт это значение из RTSP-параметра
# x-nv-video[0].packetSize и режет RTP-пакеты по нему.
PACKET_SIZE="${PACKET_SIZE:-1024}"

CLIENT_BIN="${CLIENT_BIN:-$BENCH_DIR/client/build/bench-client}"
APP_CMD="/bench/wayland-client/build/wl-shm-client $WIDTH $HEIGHT"

ARTIFACTS="${ARTIFACTS:-$BENCH_DIR/artifacts/local}"
START_TS=$(date -u +%s)

log() { printf '\n=== %s ===\n' "$*" >&2; }
die() { printf 'ОШИБКА: %s\n' "$*" >&2; exit 1; }

wolf_api() {
  local method="$1" path="$2" body="${3:-}"
  local out
  if [ -n "$body" ]; then
    local b64
    b64="$(printf '%s' "$body" | base64 | tr -d '\n')"
    out="$(printf '%s' "$b64" | base64 -d | curl -s --unix-socket "$WORK_DIR/run/wolf.sock" -X "$method" \
      -H 'Content-Type: application/json' --data-binary @- "http://localhost$path")" \
      || { printf 'wolf_api %s %s: запрос не прошёл\n' "$method" "$path" >&2; return 1; }
  else
    out="$(curl -s --unix-socket "$WORK_DIR/run/wolf.sock" -X "$method" "http://localhost$path")" \
      || { printf 'wolf_api %s %s: запрос не прошёл\n' "$method" "$path" >&2; return 1; }
  fi
  printf '%s' "$out"
}

# wolf печатает PIN секрет заглавными hex — ищем без учёта регистра
latest_pin_secret() {
  podman logs "$CONTAINER" 2>&1 | grep -aio 'pin/#[0-9a-f]*' | tail -1 | cut -d'#' -f2 || true
}

# --- потери на loopback и захват трафика --------------------------------
# Включаются отдельными командами (в этом файле нет sudo):
#   bash scripts/link-loss.sh on 1      # 1% потерь на UDP
#   bash scripts/capture.sh on out.pcap # захват UDP
#   bash scripts/link-loss.sh off
#   bash scripts/capture.sh off

cleanup() {
  local rc=$?
  exit $rc
}

build() {
  log "собираю wayland-клиент"
  "$SCRIPT_DIR/build-wayland-client.sh"
  log "собираю bench-клиент"
  "$SCRIPT_DIR/build.sh"
}

up() {
  log "поднимаю wolf в podman (net=$NET)"
  podman rm -f "$CONTAINER" >/dev/null 2>&1 || true
  rm -rf "$WORK_DIR"
  mkdir -p "$WORK_DIR/cfg" "$WORK_DIR/run" "$WORK_DIR/sock"
  chmod 700 "$WORK_DIR/run"

  sed -e "s|__APP_CMD__|$APP_CMD|" "$BENCH_DIR/local/config.toml" > "$WORK_DIR/cfg/config.toml"

  local net_args=()
  local noise_env=()
  [ "$NOISE" = "1" ] && noise_env=(-e WL_SHM_NOISE=1)
  if [ "$NET" = "host" ]; then
    net_args=(--network=host)
  else
    net_args=(-p "127.0.0.1:$HTTPS_PORT:$HTTPS_PORT/tcp"
              -p "127.0.0.1:$HTTP_PORT:$HTTP_PORT/tcp"
              -p "127.0.0.1:$VIDEO_PORT:$VIDEO_PORT/udp"
              -p "127.0.0.1:48010:48010/tcp"
              -p "127.0.0.1:$PING_VIDEO_PORT:$PING_VIDEO_PORT/udp"
              -p "127.0.0.1:$PING_AUDIO_PORT:$PING_AUDIO_PORT/udp")
  fi

  podman run -d --name "$CONTAINER" \
    --device /dev/dri \
    "${net_args[@]}" \
    -e WOLF_USE_ZERO_COPY=FALSE \
    -e WOLF_LOG_LEVEL="${WOLF_LOG_LEVEL:-DEBUG}" \
    -e GST_DEBUG="${GST_DEBUG:-2}" \
    -e WOLF_VIDEO_PING_PORT="$PING_VIDEO_PORT" \
    -e WOLF_AUDIO_PING_PORT="$PING_AUDIO_PORT" \
    "${noise_env[@]}" \
    -v "$WORK_DIR/cfg:/etc/wolf/cfg:Z" \
    -v "$WORK_DIR/run:/run/user/wolf:Z" \
    -v "$WORK_DIR/sock:/var/run/wolf:Z" \
    -v "$BENCH_DIR:/bench:Z" \
    "$IMAGE" >/dev/null

  local waited=0
  while [ "$waited" -lt 60 ]; do
    if curl -sf "http://127.0.0.1:$HTTP_PORT/serverinfo" >/dev/null 2>&1; then
      log "wolf отвечает"
      return 0
    fi
    sleep 1
    waited=$((waited + 1))
  done
  podman logs "$CONTAINER" 2>&1 | tail -20 >&2
  die "wolf не поднялся"
}

down() {
  podman rm -f "$CONTAINER" >/dev/null 2>&1 || true
  log "контейнер удалён"
}

# Один клиент: ждём пейринга (wolf не отвечает на первую фазу, пока не
# подтверждён PIN) и запускаем поток.
# PID кладём в глобальную переменную: клиент должен остаться потомком
# скрипта, иначе wait его не дождётся.
CLIENT_PID=""
run_client() {
  local name="$1" width="$2" height="$3" fps="$4" bitrate="$5"
  local identity_dir="$ARTIFACTS/identity-$name"
  rm -rf "$identity_dir"

  local before
  before="$(latest_pin_secret)"

  "$CLIENT_BIN" \
    --host "$HOST" \
    --http-port "$HTTP_PORT" \
    --https-port "$HTTPS_PORT" \
    --width "$width" --height "$height" --fps "$fps" \
    --bitrate "$bitrate" --packet-size "$PACKET_SIZE" \
    --app-title "wl-shm" \
    --pin "$PIN" \
    --identity-dir "$identity_dir" \
    --duration "$DURATION" \
    > "$ARTIFACTS/client-$name.json" 2> "$ARTIFACTS/client-$name.log" &
  CLIENT_PID=$!

  # подтверждаем PIN: каждая попытка пейринга порождает свой секрет
  local secret=""
  local waited=0
  while kill -0 "$CLIENT_PID" 2>/dev/null && [ "$waited" -lt 120 ]; do
    secret="$(latest_pin_secret)"
    if [ -n "$secret" ] && [ "$secret" != "$before" ]; then
      curl -s -X POST -H 'Content-Type: application/json' \
        -d "{\"pin\":\"$PIN\",\"secret\":\"$secret\"}" \
        "http://127.0.0.1:$HTTP_PORT/pin/" >/dev/null || true
      before="$secret"
    fi
    sleep 1
    waited=$((waited + 1))
  done
}

run() {
  mkdir -p "$ARTIFACTS"
  log "артефакты: $ARTIFACTS"

  trap cleanup EXIT

  log "запускаю клиента A"
  run_client A "$WIDTH" "$HEIGHT" "$FPS" "$BITRATE"
  local pid_a="$CLIENT_PID"

  log "запускаю клиента B"
  run_client B "$WIDTH" "$HEIGHT" "$FPS" "$BITRATE"
  local pid_b="$CLIENT_PID"

  sleep 8

  if [ "$LOBBY" = "1" ]; then
    log "создаю лобби"
    local lobby_body
    lobby_body="$(RUNNER="{\"type\":\"process\",\"run_cmd\":\"$APP_CMD\"}" \
      WIDTH="$WIDTH" HEIGHT="$HEIGHT" FPS="$FPS" python3 -c '
import json, os
print(json.dumps({
    "profile_id": "moonlight-profile-id",
    "name": "wolf-bench",
    "multi_user": True,
    "stop_when_everyone_leaves": False,
    "runner_state_folder": "profile-data/moonlight-profile-id/WolfBench",
    "runner": json.loads(os.environ["RUNNER"]),
    "video_settings": {
        "width": int(os.environ["WIDTH"]),
        "height": int(os.environ["HEIGHT"]),
        "refresh_rate": int(os.environ["FPS"]),
        "wayland_render_node": "/dev/dri/renderD128",
        "runner_render_node": "/dev/dri/renderD128",
        "video_producer_buffer_caps": "video/x-raw",
    },
    "audio_settings": {"channel_count": 2},
}))')"
    local lobby_id
    lobby_id="$(wolf_api POST /api/v1/lobbies/create "$lobby_body" | python3 -c '
import json, sys
try:
    print(json.load(sys.stdin).get("lobby_id", ""))
except Exception:
    print("")')"
    [ -n "$lobby_id" ] || die "лобби не создалось"
    log "лобби создано: $lobby_id"
    echo "$lobby_id" > "$ARTIFACTS/lobby_id"

    local sessions
    sessions="$(wolf_api GET /api/v1/sessions | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for s in data.get("sessions", []):
    print(s.get("session_id", ""))')"
    for sid in $sessions; do
      log "включаю сессию $sid в лобби"
      wolf_api POST /api/v1/lobbies/join "{\"lobby_id\":\"$lobby_id\",\"moonlight_session_id\":$sid}" >/dev/null || true
    done
  else
    log "LOBBY=0: у каждого клиента свой producer"
  fi

  log "жду $DURATION секунд"
  wait "$pid_a" 2>/dev/null || true
  wait "$pid_b" 2>/dev/null || true

  # секрет пейринга из лога не сохраняем: в артефактах ему не место
  # лог только за время прогона: иначе в счётчики попадут события
  # предыдущих прогонов (wolf перезапускается не каждый раз)
  podman logs -t --since "$START_TS" "$CONTAINER" 2>&1 \
    | sed -E 's|pin/#[0-9a-fA-F]+|pin/#<redacted>|g' > "$ARTIFACTS/wolf.log" || true

  if [ -s "$ARTIFACTS/video.pcap" ]; then
    log "разбор pcap"
    python3 "$SCRIPT_DIR/analyze.py" "$ARTIFACTS/video.pcap" --json "$ARTIFACTS/report.json" \
      > "$ARTIFACTS/analysis.txt" 2>&1 || true
    cat "$ARTIFACTS/analysis.txt"
  fi

  log "итог"
  printf 'Forcing IDR в логе wolf: %s\n' "$(grep -c 'Forcing IDR' "$ARTIFACTS/wolf.log" || true)"
  for name in A B; do
    printf -- '--- клиент %s: ' "$name"
    cat "$ARTIFACTS/client-$name.json" 2>/dev/null || echo "(нет данных)"
  done
}

case "${1:-all}" in
  build) build ;;
  up) up ;;
  run) run ;;
  down) down ;;
  all) build; up; run; down ;;
  *) die "неизвестный режим: $1" ;;
esac
