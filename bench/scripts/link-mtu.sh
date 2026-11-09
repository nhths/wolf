#!/usr/bin/env bash
# Path MTU на loopback: эмуляция «чёрной дыры» на VPN-туннеле.
#
#   link-mtu.sh on  <байт> [порт]   # включить
#   link-mtu.sh off                 # снять
#
# Что делает: роняет UDP-пакеты, длина которых (IP total length) больше
# указанного порога. Это ровно то, что происходит на пути с MTU 1420,
# когда приложение шлёт UDP-датаграмму в 1452 байта: пакет не влезает,
# а фрагментации туннель не делает — пакет исчезает целиком.
#
# Фильтр по порту-источнику: режем только видеопоток wolf'а (SOURCE_PORT),
# обратный трафик клиента (пинги) остаётся целым.
set -euo pipefail

CHAIN="WOLF_MTU"

drop_rule() {
  local mtu="$1" port="$2"
  sudo iptables -A "$CHAIN" -p udp --sport "$port" -m length --length "$((mtu + 1)):65535" -j DROP
}

case "${1:-}" in
  on)
    mtu="${2:?нужен порог MTU в байтах}"
    port="${3:-48110}"
    sudo iptables -N "$CHAIN" 2>/dev/null || true
    sudo iptables -F "$CHAIN"
    sudo iptables -C OUTPUT -j "$CHAIN" 2>/dev/null || sudo iptables -A OUTPUT -j "$CHAIN"
    drop_rule "$mtu" "$port"
    echo "режем UDP от порта $port длиннее $mtu байт (эмуляция path MTU)"
    ;;
  off)
    sudo iptables -D OUTPUT -j "$CHAIN" 2>/dev/null || true
    sudo iptables -F "$CHAIN" 2>/dev/null || true
    sudo iptables -X "$CHAIN" 2>/dev/null || true
    echo "фильтр MTU снят"
    ;;
  *)
    echo "использование: link-mtu.sh on <байт> [порт] | off" >&2
    exit 2
    ;;
esac
