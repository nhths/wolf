#!/usr/bin/env python3
"""Пишет pcap с трафиком стенда (сырой сокет AF_PACKET).

tcpdump на машине может не стоять, а для разбора артефактов нужен pcap.
Скрипт пишет классический pcap (LINKTYPE_ETHERNET) — его понимает
scripts/analyze.py.

    sudo python3 scripts/capture.py out.pcap [интерфейс] [секунды]
"""
import os
import socket
import struct
import sys
import time

PCAP_MAGIC = 0xA1B2C3D4
LINKTYPE_ETHERNET = 1


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    out_path = sys.argv[1]
    iface = sys.argv[2] if len(sys.argv) > 2 else "lo"
    duration = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0

    sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
    sock.bind((iface, 0))

    deadline = time.time() + duration if duration > 0 else None
    with open(out_path, "wb") as out:
        out.write(struct.pack("<IHHiIII", PCAP_MAGIC, 2, 4, 0, 0, 65535, LINKTYPE_ETHERNET))
        while True:
            if deadline is not None and time.time() >= deadline:
                break
            try:
                data, _addr = sock.recvfrom(65535)
            except KeyboardInterrupt:
                break
            now = time.time()
            sec = int(now)
            usec = int((now - sec) * 1_000_000)
            out.write(struct.pack("<IIII", sec, usec, len(data), len(data)))
            out.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
