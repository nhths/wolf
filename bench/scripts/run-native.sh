#!/usr/bin/env bash
# Нативный стенд: wolf запускается прямо на этой машине (без контейнера).
#
#   ./scripts/run-native.sh run     # прогон
#
# Зачем: контейнерный стенд (run-local.sh) гоняет официальный образ wolf,
# а тут нужен *патченный* wolf из форка. Видео-источник — videotestsrc с
# шумом: в контейнерном стенде кадры рисует wayland-клиент (нужен
# gst-wayland-display), нативно плагина нет.
#
# Переменные:
#   WOLF_BIN     — бинарник wolf (по умолчанию сборка форка в ../wolf-fork/build)
#   HOST         — адрес wolf для клиента; по умолчанию исходящий адрес машины.
#                  Должен быть приватным: на loopback клиент считает сервер
#                  удалённым и сам режет пакет до 1024, стенд тогда ничего не
#                  проверяет.
#   PACKET_SIZE  — размер RTP-пакета, который просит клиент (1392)
#   MTU          — порог path MTU в байтах (0 — без ограничения)
#   RULES        — "on": правило RULE_NETWORK -> RULE_SIZE, "off": без правила
#   RULE_NETWORK — сеть правила (по умолчанию <HOST>/32)
#   RULE_SIZE    — предел размера пакета в правиле (1376)
#   DURATION     — сколько секунд гонять клиент (30)
#   ARTIFACTS    — куда класть логи и pcap
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"

WOLF_BIN="${WOLF_BIN:-$BENCH_DIR/../../wolf-fork/build/src/moonlight-server/wolf}"
CLIENT_BIN="${CLIENT_BIN:-$BENCH_DIR/client/build/bench-client}"
PACKET_SIZE="${PACKET_SIZE:-1392}"
MTU="${MTU:-0}"
RULES="${RULES:-on}"
RULE_SIZE="${RULE_SIZE:-1376}"
DURATION="${DURATION:-30}"
ARTIFACTS="${ARTIFACTS:-$BENCH_DIR/artifacts/native}"
WIDTH="${WIDTH:-1280}"
HEIGHT="${HEIGHT:-720}"
FPS="${FPS:-60}"
BITRATE="${BITRATE:-20000}"
PIN="${PIN:-1234}"
HOST="${HOST:-$(ip -4 route get 8.8.8.8 2>/dev/null |
  awk '{for (i = 1; i <= NF; i++) if ($i == "src") print $(i + 1)}')}"
RULE_NETWORK="${RULE_NETWORK:-$HOST/32}"

HTTP_PORT=47989
HTTPS_PORT=47984
VIDEO_PORT=48110
AUDIO_PORT=48210

WORK_DIR="${WORK_DIR:-/tmp/wolf-native}"

log() { printf '\n=== %s ===\n' "$*" >&2; }
die() { printf 'ОШИБКА: %s\n' "$*" >&2; exit 1; }

[ -x "$WOLF_BIN" ] || die "нет бинарника wolf: $WOLF_BIN"
[ -x "$CLIENT_BIN" ] || die "нет bench-клиента: $CLIENT_BIN (собери: scripts/build.sh)"
[ -n "$HOST" ] || die "не удалось определить адрес машины, задай HOST явно"

mkdir -p "$ARTIFACTS" "$WORK_DIR/cfg" "$WORK_DIR/run" "$WORK_DIR/state"

# --- конфиг ---------------------------------------------------------------
RULE_BLOCK=""
if [ "$RULES" = "on" ]; then
  RULE_BLOCK="[[video_packet_size_rules]]
network = \"$RULE_NETWORK\"
max_packet_size = $RULE_SIZE"
fi
awk -v r="$RULE_BLOCK" '{ if ($0 == "__RULES__") { if (r != "") print r } else print }' \
  "$BENCH_DIR/local/config-native.toml" > "$WORK_DIR/cfg/config.toml"

# --- wolf ----------------------------------------------------------------
start_wolf() {
  WOLF_CFG_FILE="$WORK_DIR/cfg/config.toml" \
  WOLF_PRIVATE_KEY_FILE="$WORK_DIR/cfg/key.pem" \
  WOLF_PRIVATE_CERT_FILE="$WORK_DIR/cfg/cert.pem" \
  XDG_RUNTIME_DIR="$WORK_DIR/run" \
  HOST_APPS_STATE_FOLDER="$WORK_DIR/state" \
  WOLF_LOG_LEVEL=DEBUG \
  WOLF_VIDEO_PING_PORT="$VIDEO_PORT" \
  WOLF_AUDIO_PING_PORT="$AUDIO_PORT" \
  WOLF_USE_ZERO_COPY=FALSE \
  "$WOLF_BIN" > "$ARTIFACTS/wolf.log" 2>&1 &
  WOLF_PID=$!

  local waited=0
  while [ "$waited" -lt 30 ]; do
    curl -sf "http://127.0.0.1:$HTTP_PORT/serverinfo" >/dev/null 2>&1 && return 0
    sleep 1
    waited=$((waited + 1))
  done
  tail -20 "$ARTIFACTS/wolf.log" >&2
  die "wolf не поднялся"
}

stop_wolf() {
  [ -n "${WOLF_PID:-}" ] && kill "$WOLF_PID" 2>/dev/null || true
  wait "$WOLF_PID" 2>/dev/null || true
}

cleanup() {
  local rc=$?
  bash "$SCRIPT_DIR/link-mtu.sh" off >/dev/null 2>&1 || true
  bash "$SCRIPT_DIR/capture-local.sh" off >/dev/null 2>&1 || true
  stop_wolf
  exit $rc
}
trap cleanup EXIT

# --- клиент --------------------------------------------------------------
run_client() {
  rm -rf "$ARTIFACTS/identity"
  "$CLIENT_BIN" \
    --host "$HOST" \
    --http-port "$HTTP_PORT" \
    --https-port "$HTTPS_PORT" \
    --width "$WIDTH" --height "$HEIGHT" --fps "$FPS" \
    --bitrate "$BITRATE" --packet-size "$PACKET_SIZE" \
    --app-title "wl-shm" \
    --pin "$PIN" \
    --identity-dir "$ARTIFACTS/identity" \
    --duration "$DURATION" \
    > "$ARTIFACTS/client.json" 2> "$ARTIFACTS/client.log" &
  CLIENT_PID=$!

  # пейринг: wolf печатает секрет, клиент шлёт пин
  local before secret waited
  before="$(grep -aio 'pin/#[0-9a-f]*' "$ARTIFACTS/wolf.log" | tail -1 | cut -d'#' -f2 || true)"
  waited=0
  while kill -0 "$CLIENT_PID" 2>/dev/null && [ "$waited" -lt 60 ]; do
    secret="$(grep -aio 'pin/#[0-9a-f]*' "$ARTIFACTS/wolf.log" | tail -1 | cut -d'#' -f2 || true)"
    if [ -n "$secret" ] && [ "$secret" != "$before" ]; then
      curl -s -X POST -H 'Content-Type: application/json' \
        -d "{\"pin\":\"$PIN\",\"secret\":\"$secret\"}" \
        "http://127.0.0.1:$HTTP_PORT/pin/" >/dev/null || true
      before="$secret"
    fi
    sleep 1
    waited=$((waited + 1))
  done
  wait "$CLIENT_PID" 2>/dev/null || true
}

run() {
  log "запускаю wolf ($WOLF_BIN)"
  start_wolf

  if [ "$MTU" != "0" ]; then
    log "эмулирую path MTU $MTU"
    bash "$SCRIPT_DIR/link-mtu.sh" on "$MTU" "$VIDEO_PORT"
  fi

  log "захват трафика"
  bash "$SCRIPT_DIR/capture-local.sh" on "$ARTIFACTS/video.pcap" 0

  log "клиент просит пакет $PACKET_SIZE, правила: $RULES"
  run_client

  bash "$SCRIPT_DIR/capture-local.sh" off

  log "итог"
  printf 'клиент: %s\n' "$(cat "$ARTIFACTS/client.json" 2>/dev/null || echo '(нет данных)')"
  printf 'клампинг в логе wolf: %s\n' "$(grep -c 'Clamping video packet size' "$ARTIFACTS/wolf.log" || true)"
  printf 'payload_size в пайплайне: %s\n' \
    "$(grep -o 'payload_size=[0-9]*' "$ARTIFACTS/wolf.log" | sort | uniq -c | tr '\n' ' ')"
  if [ -s "$ARTIFACTS/video.pcap" ]; then
    python3 "$SCRIPT_DIR/analyze.py" "$ARTIFACTS/video.pcap" --json "$ARTIFACTS/report.json" \
      > "$ARTIFACTS/analysis.txt" 2>&1 || true
    grep -aE "пакет|packet|кадр|frame" "$ARTIFACTS/analysis.txt" | head -20
  fi
}

case "${1:-run}" in
  run) run ;;
  *) die "неизвестный режим: $1" ;;
esac
