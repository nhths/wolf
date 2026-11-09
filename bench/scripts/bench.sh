#!/usr/bin/env bash
# Воспроизведение шторма IDR в wolf: два клиента Moonlight в одной лобби.
#
# Лобби поднимает ОДИН видеопродюсер (waylanddisplaysrc + interpipesink),
# и все подключившиеся к ней клиенты читают из одного producer'а.
# Если продюсер не успевает — wolf выбрасывает кадры, клиент видит пропуск
# и просит IDR, wolf шлёт полный I-кадр (до bitrate/fps байт), конвейер
# отстаёт ещё сильнее — и так по кругу.
#
# Сценарий:
#   1. снять pcap видеопотока на хосте wolf;
#   2. поднять клиента A (тяжёлый поток);
#   3. создать лобби с multi_user и заданным разрешением продюсера;
#   4. поднять клиента B (лёгкий поток);
#   5. загнать обоих в лобби — теперь они делят один producer;
#   6) подождать N секунд и собрать артефакты.
#
# Требуется: ssh к хосту wolf, python3, tcpdump на хосте.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"

# ---------------------------------------------------------------- параметры
WOLF_HOST="${WOLF_HOST:-}"
WOLF_CONTAINER="${WOLF_CONTAINER:-wolf-wolf-1}"
# API wolf живёт на unix-сокете; /var/run/wolf примонтирован на хост из контейнера.
WOLF_SOCK="${WOLF_SOCK:-/var/run/wolf/wolf.sock}"
HTTP_PORT="${HTTP_PORT:-47989}"
HTTPS_PORT="${HTTPS_PORT:-47984}"
VIDEO_PORT="${VIDEO_PORT:-47999}"
# wolf отдаёт видеопоток на порту RTP-ping'а (state::VIDEO_PING_PORT,
# по умолчанию 48100), а не на VIDEO_PORT: последний — только обмен
# ping'ами. Поэтому ловим весь диапазон портов wolf'а.
WOLF_PORT_RANGE="${WOLF_PORT_RANGE:-47980-48250}"

PIN="${PIN:-1234}"
DURATION="${DURATION:-90}"

# разрешение/частота producer'а в лобби (то, что реально рендерит wolf)
PRODUCER_W="${PRODUCER_W:-2560}"
PRODUCER_H="${PRODUCER_H:-1440}"
PRODUCER_FPS="${PRODUCER_FPS:-60}"

# клиенты: ширина высота fps битрейт_кбит
CLIENT_A="${CLIENT_A:-1920 1080 60 20000}"
CLIENT_B="${CLIENT_B:-1600 900 60 15000}"

# LOBBY=0 — базовый прогон: два клиента, у каждого свой producer.
# LOBBY=1 (по умолчанию) — оба клиента в одной лобби, producer один.
LOBBY="${LOBBY:-1}"
# RFI=1 — клиент объявляет поддержку reference frame invalidation:
# вместо полного I-кадра клиент просит перерисовать только повреждённые.
RFI="${RFI:-0}"

APP_TITLE="${APP_TITLE:-Desktop (xfce)}"
CLIENT_BIN="${CLIENT_BIN:-$BENCH_DIR/client/build/bench-client}"

ARTIFACTS="${ARTIFACTS:-$BENCH_DIR/artifacts/$(date -u +%Y%m%d-%H%M%S)}"
START_TS=$(date -u +%s)
# лог wolf собираем с момента старта прогона, а не за фиксированное окно:
# иначе в счётчик «Forcing IDR» попадут запросы от предыдущего прогона.
LOG_SINCE="$START_TS"

# ------------------------------------------------------------------ утилиты
log() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*" >&2; }
die() { log "ОШИБКА: $*"; exit 1; }

require() { command -v "$1" >/dev/null 2>&1 || die "не найдено: $1"; }
[ -n "$WOLF_HOST" ] || die "укажите хост wolf: WOLF_HOST=<адрес>"

ssh_wolf() { ssh -o ConnectTimeout=10 "$WOLF_HOST" "$@"; }

# API wolf живёт на unix-сокете хоста; тело запроса передаём через base64
wolf_api() {
  local method="$1" path="$2" body="${3:-}"
  if [ -n "$body" ]; then
    local b64
    b64="$(printf '%s' "$body" | base64 | tr -d '\n')"
    ssh_wolf "echo $b64 | base64 -d | curl -s --unix-socket $WOLF_SOCK -X $method -H 'Content-Type: application/json' --data-binary @- http://localhost$path"
  else
    ssh_wolf "curl -s --unix-socket $WOLF_SOCK -X $method http://localhost$path"
  fi
}

# список id сессий wolf
session_ids() {
  wolf_api GET /api/v1/sessions | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for s in data.get("sessions", []):
    print(s.get("session_id", ""))
'
}

# ------------------------------------------------------------------ проверки
require python3
require ssh
[ -x "$CLIENT_BIN" ] || die "не собран клиент: $CLIENT_BIN (см. client/README или bench/build.sh)"

mkdir -p "$ARTIFACTS"
log "артефакты: $ARTIFACTS"

log "проверяю, что wolf на $WOLF_HOST жив"
ssh_wolf "docker ps --format '{{.Names}}' | grep -Fx '$WOLF_CONTAINER'" >/dev/null \
  || die "контейнер $WOLF_CONTAINER не запущен на $WOLF_HOST"

# ------------------------------------------------------------------ pcap
PCAP_REMOTE="/tmp/wolfvid-$$.pcap"
log "включаю захват видеопотока -> $PCAP_REMOTE"
ssh_wolf "rm -f $PCAP_REMOTE; nohup tcpdump -ni any -s 0 -U -w $PCAP_REMOTE 'udp portrange $WOLF_PORT_RANGE' >/dev/null 2>&1 &" || die "не удалось запустить tcpdump"
sleep 2

cleanup() {
  local rc=$?
  log "останавливаю захват"
  ssh_wolf "pkill -f $PCAP_REMOTE" || true
  log "выгружаю артефакты"
  ssh_wolf "docker logs -t $WOLF_CONTAINER --since $LOG_SINCE" > "$ARTIFACTS/wolf.log" 2>&1 || true
  ssh_wolf "cat $PCAP_REMOTE" > "$ARTIFACTS/video.pcap" 2>/dev/null || true
  ssh_wolf "rm -f $PCAP_REMOTE" || true
  log "готово: $ARTIFACTS"
  exit $rc
}
trap cleanup EXIT

# ------------------------------------------------------------------ пейринг
# wolf публикует PIN в своём логе: "Insert pin at http://ip:port/pin/#<secret>".
# Секрет отдаём обратно через POST /pin/ — тогда пейринг полностью автоматический.
#
# Каждой попытке пейринга соответствует свой secret, и он тратится сразу после
# использования. Поэтому засекаем secret ДО старта клиента и ждём другой —
# иначе для второго клиента можно отправить уже потраченный secret от первого.
latest_pin_secret() {
  # wolf печатает secret заглавными hex — ищем без учёта регистра,
  # иначе в логе не найдётся ничего
  ssh_wolf "docker logs -t $WOLF_CONTAINER --since $LOG_SINCE 2>&1" \
    | grep -aio 'pin/#[0-9a-f]*' | tail -1 | cut -d'#' -f2 || true
}

send_pin() {
  local before="${1:-}"
  local secret=""
  for _ in $(seq 1 120); do
    secret="$(latest_pin_secret)"
    if [ -n "$secret" ] && [ "$secret" != "$before" ]; then
      break
    fi
    sleep 1
  done
  if [ -z "$secret" ] || [ "$secret" = "$before" ]; then
    return 1
  fi
  log "отправляю PIN $PIN (secret $secret)"
  curl -s -X POST -H 'Content-Type: application/json' \
    -d "{\"pin\":\"$PIN\",\"secret\":\"$secret\"}" \
    "http://$WOLF_HOST:$HTTP_PORT/pin/" >/dev/null
}

# ------------------------------------------------------------------ запуск
# PID кладём в глобальную переменную: если запускать клиента в подстановке
# команды, он окажется потомком подshell, и wait его не дождётся.
CLIENT_PID=""
run_client() {
  local name="$1" width="$2" height="$3" fps="$4" bitrate="$5"
  local identity="${width}x${height}_${fps}"
  local identity_dir="$ARTIFACTS/identity-$identity"
  # каждый прогон — свежая идентичность: uniqueid должен быть новым, иначе
  # wolf отвечает «Out of order pair request (phase 1)» (кэш пейринга живёт
  # в памяти и переживает неудачную попытку)
  rm -rf "$identity_dir"
  local before
  before="$(latest_pin_secret)"

  log "запускаю клиент $name: ${width}x${height}@${fps} ${bitrate} Кбит/с"
  local rfi_flag=()
  [ "$RFI" = "1" ] && rfi_flag=(--rfi)
  "$CLIENT_BIN" \
    --host "$WOLF_HOST" \
    --http-port "$HTTP_PORT" \
    --https-port "$HTTPS_PORT" \
    --width "$width" --height "$height" --fps "$fps" \
    --bitrate "$bitrate" --packet-size 1024 \
    --app-title "$APP_TITLE" \
    --pin "$PIN" \
    --identity-dir "$identity_dir" \
    --duration "$DURATION" \
    "${rfi_flag[@]}" \
    > "$ARTIFACTS/client-$name.json" 2> "$ARTIFACTS/client-$name.log" &

  CLIENT_PID=$!
  # пейринг: wolf не отвечает на первую фазу, пока не введён PIN
  if ! send_pin "$before"; then
    log "не дождался PIN для $name"
  fi
}

run_client A $CLIENT_A
client_pid_a="$CLIENT_PID"
run_client B $CLIENT_B
client_pid_b="$CLIENT_PID"

# ждём, пока wolf создаст сессии
sleep 6
sessions_now="$(session_ids | sort -n)"
log "сессии wolf: $(echo "$sessions_now" | tr '\n' ' ')"

# ------------------------------------------------------------------ лобби
# Создаём лобби с multi_user — оба клиента будут читать из одного producer'а.
lobby_id=""
if [ "$LOBBY" != "1" ]; then
  log "LOBBY=0: базовый прогон — у каждого клиента свой producer, лобби не создаём"
else
  profile_json="$(wolf_api GET /api/v1/profiles)"
  runner_json="$(printf '%s' "$profile_json" | python3 -c '
import json, sys
data = json.load(sys.stdin)
for profile in data.get("profiles", []):
    for app in profile.get("apps", []):
        title = app.get("title", "")
        runner = app.get("runner")
        if runner and "xfce" in title.lower():
            print(json.dumps(runner))
            sys.exit(0)
sys.exit(1)
' || true)"

  if [ -z "$runner_json" ]; then
    log "не нашёл runner для «$APP_TITLE» в профилях — лобби не создам, продолжу без него"
  else
    export PRODUCER_W PRODUCER_H PRODUCER_FPS
    lobby_body="$(RUNNER="$runner_json" python3 -c '
import json, os
runner = json.loads(os.environ["RUNNER"])
print(json.dumps({
    "profile_id": "user",
    "name": "wolf-bench",
    "multi_user": True,
    "stop_when_everyone_leaves": False,
    "runner_state_folder": "profile-data/user/WolfBench",
    "runner": runner,
    "video_settings": {
        "width": int(os.environ.get("PRODUCER_W", 2560)),
        "height": int(os.environ.get("PRODUCER_H", 1440)),
        "refresh_rate": int(os.environ.get("PRODUCER_FPS", 60)),
        "wayland_render_node": "/dev/dri/renderD128",
        "runner_render_node": "/dev/dri/renderD128",
        "video_producer_buffer_caps": "video/x-raw(memory:CUDAMemory)",
    },
    "audio_settings": {"channel_count": 2},
}))
' )"
    lobby_id="$(wolf_api POST /api/v1/lobbies/create "$lobby_body" | python3 -c '
import json, sys
data = json.load(sys.stdin)
print(data.get("lobby_id", ""))
')"
    [ -n "$lobby_id" ] || die "лобби не создалось"
    log "лобби создано: $lobby_id"
    echo "$lobby_id" > "$ARTIFACTS/lobby_id"

    # ---------------------------------------------------------------- вход в лобби
    sessions_now="$(session_ids | sort -n)"
    for sid in $sessions_now; do
      log "включаю сессию $sid в лобби"
      wolf_api POST /api/v1/lobbies/join "{\"lobby_id\":\"$lobby_id\",\"moonlight_session_id\":$sid}" >/dev/null || true
    done
  fi
fi

log "жду $DURATION секунд"
wait "$client_pid_a" 2>/dev/null || true
wait "$client_pid_b" 2>/dev/null || true

# ------------------------------------------------------------------ разбор
log "анализирую pcap"
# порт видеопотока определяется автоматически: в wolf он зависит от
# WOLF_VIDEO_PING_PORT, а не от фиксированного значения
python3 "$SCRIPT_DIR/analyze.py" "$ARTIFACTS/video.pcap" \
  --json "$ARTIFACTS/report.json" | tee "$ARTIFACTS/analysis.txt"

log "сводка по логу wolf: $(grep -c 'Forcing IDR' "$ARTIFACTS/wolf.log" || true) запросов IDR"
