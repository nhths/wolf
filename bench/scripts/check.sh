#!/usr/bin/env bash
# Проверка связи со стендом без потока: пейринг + список приложений.
#
# Клиент висит на первой фазе пейринга, пока wolf не получит PIN.
# Поэтому PIN подставляется отдельным фоновым циклом: он сам следит за
# логом wolf и подтверждает каждый новый secret.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"

WOLF_HOST="${WOLF_HOST:-<адрес wolf>}"
WOLF_CONTAINER="${WOLF_CONTAINER:-wolf-wolf-1}"
HTTP_PORT="${HTTP_PORT:-47989}"
HTTPS_PORT="${HTTPS_PORT:-47984}"
PIN="${PIN:-1234}"
WATCH_SECONDS="${WATCH_SECONDS:-90}"
IDENTITY_DIR="${IDENTITY_DIR:-$BENCH_DIR/identity-check}"
CLIENT_BIN="${CLIENT_BIN:-$BENCH_DIR/client/build/bench-client}"

log() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*" >&2; }
die() { log "ОШИБКА: $*"; exit 1; }

[ -x "$CLIENT_BIN" ] || die "не собран клиент: $CLIENT_BIN"

# Свежая идентичность: uniqueid должен быть новым, иначе wolf отвечает
# «Out of order pair request (phase 1)» — кэш пейринга переживает
# неудачную попытку.
rm -rf "$IDENTITY_DIR"
mkdir -p "$IDENTITY_DIR"

log "запускаю клиента: пейринг + список приложений"
"$CLIENT_BIN" \
  --host "$WOLF_HOST" \
  --http-port "$HTTP_PORT" \
  --https-port "$HTTPS_PORT" \
  --app-title "${APP_TITLE:-Desktop (xfce)}" \
  --pin "$PIN" \
  --identity-dir "$IDENTITY_DIR" \
  --check \
  > "$IDENTITY_DIR/client.out" 2> "$IDENTITY_DIR/client.log" &
CLIENT_PID=$!

# Подтверждаем PIN: цикл ловит каждый новый secret в логе wolf.
last_secret=""
deadline=$(( $(date -u +%s) + WATCH_SECONDS ))
while kill -0 "$CLIENT_PID" 2>/dev/null && [ "$(date -u +%s)" -lt "$deadline" ]; do
  secret="$(ssh -o ConnectTimeout=10 root@"$WOLF_HOST" \
    "docker logs $WOLF_CONTAINER 2>&1" 2>/dev/null \
    | grep -aio 'pin/#[0-9a-f]*' | tail -1 | cut -d'#' -f2 | tr -d '\r\n' || true)"
  if [ -n "$secret" ] && [ "$secret" != "$last_secret" ]; then
    log "подтверждаю PIN"
    curl -s -X POST -H 'Content-Type: application/json' \
      -d "{\"pin\":\"$PIN\",\"secret\":\"$secret\"}" \
      "http://$WOLF_HOST:$HTTP_PORT/pin/" >/dev/null || true
    last_secret="$secret"
  fi
  sleep 2
done

if kill -0 "$CLIENT_PID" 2>/dev/null; then
  log "клиент не завершился за $WATCH_SECONDS с — останавливаю"
  kill "$CLIENT_PID" 2>/dev/null || true
  wait "$CLIENT_PID" 2>/dev/null || true
  cat "$IDENTITY_DIR/client.log" >&2
  exit 1
fi

wait "$CLIENT_PID" && rc=0 || rc=$?
log "клиент завершился с кодом $rc"
echo "--- stdout:"; cat "$IDENTITY_DIR/client.out"
echo "--- stderr:"; cat "$IDENTITY_DIR/client.log"
exit "$rc"
