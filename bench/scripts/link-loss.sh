#!/usr/bin/env bash
# Потери на loopback: эмуляция «VPN-линка» для локального стенда.
#
#   link-loss.sh on  <процент>   # включить
#   link-loss.sh off              # снять
#
# Теряются только UDP-пакеты: HTTP/Rtsp идут по TCP, им потери не нужны.
# ENet (управление клиента) тоже UDP, но он сам переспрашивает потерянное.
set -euo pipefail

IFACE="${IFACE:-lo}"
LOSS="${1:-}"

case "${2:-on}" in
  on)
    percent="${LOSS:-1}"
    sudo tc qdisc del dev "$IFACE" root 2>/dev/null || true
    sudo tc qdisc add dev "$IFACE" root handle 1: prio bands 3
    sudo tc qdisc add dev "$IFACE" parent 1:3 handle 30: netem loss "${percent}%"
    sudo tc filter add dev "$IFACE" protocol ip parent 1:0 prio 3 u32 match ip protocol 17 0xff flowid 1:3
    echo "netem на $IFACE: потери ${percent}% (UDP)"
    ;;
  off)
    sudo tc qdisc del dev "$IFACE" root 2>/dev/null || true
    echo "netem снят с $IFACE"
    ;;
  *)
    echo "использование: link-loss.sh on|off [процент]" >&2
    exit 2
    ;;
esac
