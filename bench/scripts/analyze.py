#!/usr/bin/env python3
"""Разбирает pcap с видеопотоком Moonlight и ищет признаки шторма IDR.

Формат видеопакета wolf (совпадает с moonlight-common-c):

    [0]      RTP header  (0x80 | 0x10 = версия 2 + бит расширения)
    [1]      packetType
    [2:4]    sequenceNumber (big-endian)
    [4:8]    timestamp
    [8:12]   ssrc
    [12:16]  заполнитель расширения RTP
    [16:20]  streamPacketIndex (little-endian)
    [20:24]  frameIndex        (little-endian)
    [24]     flags: 0x1 CONTAINS_PIC_DATA, 0x2 EOF, 0x4 SOF
    [25]     reserved
    [26]     multiFecFlags
    [27]     multiFecBlocks
    [28:32]  fecInfo (little-endian)
    [32:40]  VideoShortHeader:
                 [32]     header_type = 0x01
                 [33:35]  unknown
                 [35]     frame_type: 1 = P-кадр, 2 = IDR
                 [36:38]  last_payload_len (little-endian)
                 [38:40]  unknown
    [40:]    полезная нагрузка (H.264/HEVC NAL-юниты) + нулевое дополнение

Кадр может занимать несколько RTP-пакетов: SOF стоит на первом, EOF — на
последнем. FEC-пакеты (parity shards) не несут картинки: у них бит
CONTAINS_PIC_DATA снят, и в анализ кадра не попадают.

Видео wolf не шифруется, поэтому в pcap видно всё: номера кадров, типы
кадров и сами пропуски.

Каждый клиент получает СВОЙ RTP-поток (у каждого конвейера свой
rtpmoonlightpay и свой счётчик frameIndex), поэтому потоки разбираются
отдельно — по паре портов.

Использование:
    analyze.py <file.pcap> [--port 47999] [--json out.json]
"""
import argparse
import json
import struct
import sys
from collections import defaultdict

FLAG_CONTAINS_PIC_DATA = 0x1
FLAG_EOF = 0x2
FLAG_SOF = 0x4

# Бит расширения в первом байте RTP-заголовка. После заголовка идёт
# 4-байтный заголовок расширения, и только за ним — NV-заголовок.
FLAG_RTP_EXTENSION = 0x10

# Смещения внутри пакета wolf.
OFF_NV = 16          # начало NV_VIDEO_PACKET
OFF_FRAME_INDEX = 20
OFF_FLAGS = 24
OFF_MULTI_FEC_FLAGS = 26
OFF_FEC_INFO = 28
OFF_SHORT = 32       # начало VideoShortHeader
OFF_FRAME_TYPE = 35
OFF_LAST_LEN = 36
OFF_PAYLOAD = 40     # начало полезной нагрузки кадра

# multiFecFlags wolf всегда ставит 0x10 — это надёжный признак
# «это пакет видеопотока wolf», а не ping или служебный обмен.
WOLF_MULTI_FEC_FLAGS = 0x10

MIN_VIDEO_PKT = OFF_PAYLOAD

# NAL-типы: H.264 — 5 (IDR), HEVC — 19/20 (IDR), 21 (CRA)
IDR_NAL_TYPES = {5, 19, 20, 21}

# Окно, в котором соседние пропуски считаются одним эпизодом потерь.
LOSS_WINDOW_S = 1.0


def iter_pcap(path):
    """Отдаёт (ts, linktype, raw_packet) из classic pcap."""
    with open(path, "rb") as f:
        header = f.read(24)
        if len(header) < 24:
            raise ValueError("файл слишком короткий для pcap")
        magic = header[:4]
        if magic == b"\xd4\xc3\xb2\xa1":
            endian = "<"
        elif magic == b"\xa1\xb2\xc3\xd4":
            endian = ">"
        elif magic == b"\x4d\x3c\xb2\xa1":
            endian = "<"
        else:
            raise ValueError(f"неизвестный формат pcap: {magic!r}")

        linktype = struct.unpack(endian + "I", header[20:24])[0]

        while True:
            rec = f.read(16)
            if len(rec) < 16:
                break
            ts_sec, ts_usec, incl_len, _orig_len = struct.unpack(endian + "IIII", rec)
            data = f.read(incl_len)
            if len(data) < incl_len:
                break
            yield ts_sec + ts_usec / 1e6, linktype, data


def parse_udp(linktype, raw):
    """Достаёт (src_port, dst_port, payload) из кадра или None."""
    if linktype == 1:  # Ethernet
        if len(raw) < 14:
            return None
        ethertype = struct.unpack("!H", raw[12:14])[0]
        offset = 14
        while ethertype in (0x8100, 0x88A8):  # VLAN
            if len(raw) < offset + 4:
                return None
            ethertype = struct.unpack("!H", raw[offset + 2 : offset + 4])[0]
            offset += 4
        if ethertype != 0x0800:
            return None
    elif linktype == 101:  # raw IP
        offset = 0
    elif linktype == 113:  # Linux cooked
        if len(raw) < 16:
            return None
        offset = 16
    else:
        return None

    if len(raw) < offset + 20:
        return None
    version_ihl = raw[offset]
    if version_ihl >> 4 != 4:
        return None
    ihl = (version_ihl & 0x0F) * 4
    if len(raw) < offset + ihl + 8:
        return None
    if raw[offset + 9] != 17:  # не UDP
        return None
    src_port, dst_port = struct.unpack("!HH", raw[offset + ihl : offset + ihl + 4])
    udp_len = struct.unpack("!H", raw[offset + ihl + 4 : offset + ihl + 6])[0]
    payload_start = offset + ihl + 8
    payload_end = offset + ihl + udp_len if udp_len else len(raw)
    payload = raw[payload_start:payload_end]
    return src_port, dst_port, payload


def parse_video_packet(payload):
    """Достаёт поля видеопакета wolf или None, если это не видео.

    Отличает пакет с картинкой от FEC-пакета (parity shard): в fecInfo
    лежит номер шарда и число шардов с данными, шарды с данными идут
    первыми.
    """
    if len(payload) < MIN_VIDEO_PKT:
        return None
    if payload[0] & 0xF0 != 0x90:  # RTP v2 + бит расширения
        return None
    if payload[OFF_MULTI_FEC_FLAGS] != WOLF_MULTI_FEC_FLAGS:
        return None

    fec_info = struct.unpack("<I", payload[OFF_FEC_INFO : OFF_FEC_INFO + 4])[0]
    data_shards = fec_info >> 22
    shard_index = (fec_info & 0x3FF000) >> 12
    if data_shards == 0 or shard_index >= data_shards:
        return None  # parity-шард FEC, картинки в нём нет

    flags = payload[OFF_FLAGS]
    frame_index = struct.unpack("<I", payload[OFF_FRAME_INDEX : OFF_FRAME_INDEX + 4])[0]
    seq = struct.unpack("!H", payload[2:4])[0]

    return {
        "frame_index": frame_index,
        "seq": seq,
        "flags": flags,
        "sof": bool(flags & FLAG_SOF),
        "eof": bool(flags & FLAG_EOF),
        "frame_type": payload[OFF_FRAME_TYPE],
        "last_len": struct.unpack("<H", payload[OFF_LAST_LEN : OFF_LAST_LEN + 2])[0],
        "chunk": payload[OFF_SHORT:],
    }


def contains_idr(data):
    """Ищет в полезной нагрузке NAL-юнит IDR (H.264/HEVC)."""
    i = 0
    n = len(data)
    while i + 3 < n:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                nal = data[i + 3]
                if (nal >> 1) & 0x3F in IDR_NAL_TYPES:
                    return True
                i += 3
                continue
            if i + 4 < n and data[i + 2] == 0 and data[i + 3] == 1:
                nal = data[i + 4]
                if (nal >> 1) & 0x3F in IDR_NAL_TYPES:
                    return True
                i += 4
                continue
        i += 1
    return False


class Stream:
    """Один RTP-поток видео — обычно один клиент."""

    def __init__(self, src_port, dst_port):
        self.src_port = src_port
        self.dst_port = dst_port
        self.packets = 0
        self.frames = {}  # frameIndex -> dict
        self._seqs = set()
        self._last_index = -1
        self.pkt_len = None

    def accept_length(self, length, sof):
        """Все пакеты видеопотока wolf одинаковой длины (чанк + заголовки).

        Это отсекает ping/служебные пакеты, случайно подходящие под
        формат NV-заголовка. Длина «заселяется» первым пакетом кадра
        (SOF), до него поток считается невидео.
        """
        if self.pkt_len is None:
            if not sof:
                return False
            self.pkt_len = length
            return True
        return self.pkt_len == length

    def is_duplicate(self, seq):
        """loopback отдаёт каждый пакет дважды — по sequenceNumber отсекаем."""
        if seq in self._seqs:
            return True
        self._seqs.add(seq)
        return False

    def add(self, ts, pkt):
        self.packets += 1
        index = pkt["frame_index"]
        frame = self.frames.get(index)
        if frame is None:
            if not pkt["sof"] and index <= self._last_index:
                return False  # пакет от кадра, который мы уже закрыли
            frame = self.frames[index] = {
                "index": index,
                "chunks": [],
                "packets": 0,
                "first_ts": ts,
                "last_ts": ts,
                "last_len": None,
                "frame_type": None,
                "partial": not pkt["sof"],
            }
            self._last_index = index
        # кадр режется на чанки по payload_size; последний чанк короче,
        # его длина записана в VideoShortHeader первого пакета кадра
        chunk = pkt["chunk"]
        if pkt["eof"] and frame["last_len"]:
            chunk = chunk[: frame["last_len"]]
        frame["chunks"].append(chunk)
        frame["packets"] += 1
        frame["last_ts"] = ts
        if pkt["sof"]:
            frame["frame_type"] = pkt["frame_type"]
            frame["last_len"] = pkt["last_len"]
        return True

    def finalize(self):
        """Считает размер кадра и признак IDR по собранным чанкам."""
        for frame in self.frames.values():
            data = b"".join(frame["chunks"])
            # первые 8 байт кадра — VideoShortHeader
            frame["data"] = data[8:]
            frame["size"] = len(frame["data"])
            frame["idr"] = frame["frame_type"] == 2 or contains_idr(frame["data"])

    def ordered(self):
        return [self.frames[k] for k in sorted(self.frames)]

    def gaps(self):
        out = []
        for prev, cur in zip(self.ordered(), self.ordered()[1:]):
            missing = cur["index"] - prev["index"] - 1
            if missing > 0:
                out.append({"after_frame": prev["index"], "missing": missing, "ts": cur["first_ts"]})
        return out

    def idr_frames(self):
        return [f for f in self.ordered() if f["idr"]]

    def loss_windows(self):
        """Склеивает соседние пропуски в эпизоды потерь."""
        windows = []
        for gap in self.gaps():
            if windows and gap["ts"] - windows[-1]["end"] <= LOSS_WINDOW_S:
                windows[-1]["end"] = gap["ts"]
                windows[-1]["missing"] += gap["missing"]
                windows[-1]["gaps"] += 1
            else:
                windows.append(
                    {"start": gap["ts"], "end": gap["ts"], "missing": gap["missing"], "gaps": 1}
                )
        return windows


def common_windows(streams):
    """Окна потерь, которые встретились у нескольких клиентов одновременно.

    Это и есть признак лобби: один producer завален, и кадры теряют все,
    кто из него читает.
    """
    if len(streams) < 2:
        return []
    per_stream = [s.loss_windows() for s in streams]
    result = []
    for windows in per_stream:
        for w in windows:
            for other in per_stream:
                if other is windows:
                    continue
                if any(o["start"] <= w["end"] and w["start"] <= o["end"] for o in other):
                    result.append(w)
                    break
    unique = []
    for w in result:
        if not any(abs(w["start"] - u["start"]) < 0.001 for u in unique):
            unique.append(w)
    return unique


def describe(stream, lines):
    ordered = stream.ordered()
    gaps = stream.gaps()
    idr_frames = stream.idr_frames()
    total_bytes = sum(f["size"] for f in ordered)
    span = ordered[-1]["last_ts"] - ordered[0]["first_ts"] if len(ordered) > 1 else 0.0

    lines.append(f"=== поток {stream.src_port} -> {stream.dst_port} ===")
    if span > 0:
        lines.append(
            f"  пакетов: {stream.packets}, кадров: {len(ordered)}, интервал: {span:.1f} с, "
            f"скорость: {total_bytes * 8 / span / 1e6:.1f} Мбит/с"
        )
    else:
        lines.append(f"  пакетов: {stream.packets}, кадров: {len(ordered)}")
    if idr_frames and span > 0:
        lines.append(f"  I-кадров: {len(idr_frames)} ({len(idr_frames) / span:.2f} в секунду)")
    else:
        lines.append(f"  I-кадров: {len(idr_frames)}")
    lines.append(f"  пропущено кадров: {sum(g['missing'] for g in gaps)} в {len(gaps)} местах")

    if idr_frames:
        sizes = sorted((f["size"] for f in idr_frames), reverse=True)
        biggest = sorted(idr_frames, key=lambda f: -f["size"])[:5]
        lines.append(
            f"  I-кадры: медиана {sizes[len(sizes) // 2] / 1024:.1f} КБ, "
            f"максимум {sizes[0] / 1024:.1f} КБ"
        )
        for frame in biggest:
            lines.append(
                f"    кадр {frame['index']:>8}  {frame['size'] / 1024:8.1f} КБ  {frame['packets']:>5} пакетов"
            )

    bursts = []
    current = []
    for frame in idr_frames:
        if current and frame["first_ts"] - current[-1]["first_ts"] > 1.0:
            bursts.append(current)
            current = []
        current.append(frame)
    if current:
        bursts.append(current)
    big = [b for b in bursts if len(b) >= 3]
    if big:
        lines.append(f"  всплески I-кадров (>=3 за секунду): {len(big)}")
        for burst in big[:10]:
            lines.append(
                f"    {burst[0]['first_ts']:.3f} .. {burst[-1]['first_ts']:.3f}: "
                f"{len(burst)} I-кадров, {sum(f['size'] for f in burst) / 1e6:.1f} МБ"
            )
    if gaps:
        lines.append("  первые пропуски:")
        for gap in gaps[:10]:
            lines.append(f"    после кадра {gap['after_frame']}: потеряно {gap['missing']}")
    lines.append("")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("pcap")
    parser.add_argument(
        "--port",
        type=int,
        default=None,
        help="UDP-порт видео (по умолчанию определяется автоматически)",
    )
    parser.add_argument("--json", default=None, help="выгрузить отчёт в JSON")
    args = parser.parse_args()

    streams = {}
    fec_total = 0
    other = 0
    orphan = 0

    for ts, linktype, raw in iter_pcap(args.pcap):
        parsed = parse_udp(linktype, raw)
        if not parsed:
            continue
        src_port, dst_port, payload = parsed
        if args.port is not None and src_port != args.port and dst_port != args.port:
            continue
        if (
            len(payload) < MIN_VIDEO_PKT
            or payload[0] & 0xF0 != 0x90
            or payload[OFF_MULTI_FEC_FLAGS] != WOLF_MULTI_FEC_FLAGS
        ):
            other += 1
            continue

        video = parse_video_packet(payload)
        if not video:
            # видеоподобный пакет без картинки: FEC-пакет (parity shard)
            fec_total += 1
            continue

        key = (src_port, dst_port)
        stream = streams.get(key)
        if stream is None:
            stream = streams[key] = Stream(src_port, dst_port)
        if stream.is_duplicate(video["seq"]):
            continue
        if not stream.accept_length(len(payload), video["sof"]):
            other += 1
            continue
        if not stream.add(ts, video):
            orphan += 1

    if not streams:
        print("в pcap не найдено ни одного видеопакета")
        return 1

    ordered_streams = [streams[k] for k in sorted(streams) if streams[k].frames]
    for stream in ordered_streams:
        stream.finalize()

    lines = []
    lines.append(f"файл: {args.pcap}")
    lines.append(
        f"потоков видео: {len(ordered_streams)}, FEC-пакетов: {fec_total}, "
        f"пакетов вне кадра: {orphan}"
    )
    lines.append("")

    for stream in ordered_streams:
        describe(stream, lines)

    shared = common_windows(ordered_streams)
    if shared:
        lines.append("=== общие окна потерь (несколько клиентов одновременно) ===")
        lines.append("    Это признак лобби: один producer не успевает, кадры теряют все.")
        for w in shared[:20]:
            lines.append(
                f"    {w['start']:.3f} .. {w['end']:.3f}: потеряно {w['missing']} кадров"
            )
        lines.append("")

    print("\n".join(lines))

    report = {
        "streams": [
            {
                "src_port": s.src_port,
                "dst_port": s.dst_port,
                "packets": s.packets,
                "frames": len(s.frames),
                "idr_frames": len(s.idr_frames()),
                "lost_frames": sum(g["missing"] for g in s.gaps()),
                "gap_count": len(s.gaps()),
                "max_idr_bytes": max((f["size"] for f in s.idr_frames()), default=0),
            }
            for s in ordered_streams
        ],
        "shared_loss_windows": [{"start": w["start"], "end": w["end"], "missing": w["missing"]} for w in shared],
    }
    if args.json:
        with open(args.json, "w") as f:
            json.dump(report, f, indent=2)
        print(f"отчёт: {args.json}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
