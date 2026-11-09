#!/usr/bin/env bash
# Захват UDP-трафика стенда в pcap.
#
#   capture-local.sh on <файл.pcap> [секунд]   # начать
#   capture-local.sh off                       # остановить
#
# tcpdump на машине может не стоять, поэтому пишем pcap сами
# (scripts/capture.py, сырой сокет AF_PACKET).
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IFACE="${IFACE:-lo}"
PIDFILE="${PIDFILE:-/tmp/wolf-bench-capture.pid}"

case "${1:-}" in
  on)
    file="${2:?нужен путь к pcap}"
    seconds="${3:-0}"
    sudo python3 "$SCRIPT_DIR/capture.py" "$file" "$IFACE" "$seconds" >/dev/null 2>&1 &
    echo $! > "$PIDFILE"
    sleep 1
    echo "захват идёт: $file (pid $(cat "$PIDFILE"))"
    ;;
  off)
    if [ -f "$PIDFILE" ]; then
      sudo kill "$(cat "$PIDFILE")" 2>/dev/null || true
      rm -f "$PIDFILE"
    fi
    echo "захват остановлен"
    ;;
  *)
    echo "использование: capture-local.sh on <файл.pcap> [секунд] | off" >&2
    exit 2
    ;;
esac
