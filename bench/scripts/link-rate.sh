#!/usr/bin/env bash
# Ограничение полосы на loopback: эмуляция медленного аплинка.
#
#   link-rate.sh on <китбит/с> [порт]   # включить
#   link-rate.sh off                    # снять
#
# Смысл: клиент просит битрейт, wolf его честно отдаёт, но канал
# узкий. Тогда wolf не может отправить все RTP-пакеты, appsink
# (max-buffers=1, drop=true) выбрасывает их — и уже пронумерованные
# кадры теряются. Клиент видит пропуск в номерах кадров и просит IDR.
set -euo pipefail

case "${1:-}" in
  on)
    RATE_KBIT="${2:?нужен битрейт в кбит/с}"
    SOURCE_PORT="${3:-48110}"
    sudo tc qdisc del dev lo root 2>/dev/null || true
    sudo tc qdisc add dev lo root handle 1: prio bands 3
    sudo tc qdisc add dev lo parent 1:3 handle 30: \
      tbf rate "${RATE_KBIT}kbit" burst 64kbit latency 400ms
    sudo tc filter add dev lo protocol ip parent 1:0 prio 3 \
      u32 match ip protocol 17 0xff match ip sport "$SOURCE_PORT" 0xffff flowid 1:3
    echo "канал wolf->клиент ограничен ${RATE_KBIT} кбит/с (порт $SOURCE_PORT)"
    ;;
  off)
    sudo tc qdisc del dev lo root 2>/dev/null || true
    echo "ограничение полосы снято"
    ;;
  *)
    echo "использование: link-rate.sh on <китбит/с> [порт] | off" >&2
    exit 2
    ;;
esac
