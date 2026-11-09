#!/usr/bin/env bash
# Оркестратор стенда: сборка клиента, три прогона, сводка для тикета.
#
#   ./scripts/run-all.sh build     # собрать клиент (moonlight-common-c через FetchContent)
#   ./scripts/run-all.sh run       # три прогона: базовый / лобби / лобби+RFI
#   ./scripts/run-all.sh summary   # сводная таблица + подстановка в TICKET.md
#   ./scripts/run-all.sh all       # build + run + summary
#
# Публикация стенда в форк — отдельный скрипт scripts/publish-to-fork.sh.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"

WOLF_HOST="${WOLF_HOST:-<адрес wolf>}"
DURATION="${DURATION:-90}"
ARTIFACTS_ROOT="${ARTIFACTS_ROOT:-$BENCH_DIR/artifacts}"

# прогоны: имя LOBBY RFI подпись
RUNS=(
  "baseline 0 0 базовый (два producer'а)"
  "lobby 1 0 лобби (один producer)"
  "lobby-rfi 1 1 лобби + RFI"
)

log() { printf '\n=== %s ===\n' "$*" >&2; }
die() { printf 'ОШИБКА: %s\n' "$*" >&2; exit 1; }

build() {
  log "собираю клиент"
  "$BENCH_DIR/scripts/build.sh"
}

run() {
  local name lobby rfi label
  for entry in "${RUNS[@]}"; do
    read -r name lobby rfi label <<<"$entry"
    log "прогон $name: $label"
    ARTIFACTS="$ARTIFACTS_ROOT/$name" \
      WOLF_HOST="$WOLF_HOST" \
      DURATION="$DURATION" \
      LOBBY="$lobby" \
      RFI="$rfi" \
      "$BENCH_DIR/scripts/bench.sh" || die "прогон $name упал"
  done
}

summary() {
  log "собираю сводку"
  python3 "$SCRIPT_DIR/summarize.py" --artifacts "$ARTIFACTS_ROOT" --ticket "$BENCH_DIR/TICKET.md"
}

publish() {
  log "публикую стенд в форк"
  "$SCRIPT_DIR/publish-to-fork.sh" "${FORK_URL:-https://github.com/nhths/wolf.git}" "${BRANCH:-feat/testbench}"
}

case "${1:-all}" in
  build) build ;;
  run) run ;;
  summary) summary ;;
  publish) publish ;;
  all) build; run; summary ;;
  *) die "неизвестный режим: $1" ;;
esac
