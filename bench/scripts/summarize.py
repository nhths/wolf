#!/usr/bin/env python3
"""Сводит результаты прогонов стенда в таблицу и подставляет её в TICKET.md.

Читает каталоги артефактов (каждый — результат одного прогона):
    <artifacts>/<прогон>/report.json    разбор pcap
    <artifacts>/<прогон>/wolf.log       лог контейнера wolf
    <artifacts>/<прогон>/client-*.json  метрики клиентов

Использование:
    summarize.py --artifacts DIR [--ticket FILE]
"""
import argparse
import json
import os
import re
import sys

RUNS = [
    ("baseline", "базовый (два producer'а)"),
    ("lobby", "лобби (один producer)"),
    ("lobby-rfi", "лобби + RFI"),
]


def load_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:
        return None


def read_run(directory):
    """Достаёт сводные числа одного прогона."""
    out = {"name": os.path.basename(directory)}

    report = load_json(os.path.join(directory, "report.json")) or {}
    streams = report.get("streams", [])
    out["lost_frames"] = sum(s.get("lost_frames", 0) for s in streams)
    out["idr_frames"] = sum(s.get("idr_frames", 0) for s in streams)
    out["gaps"] = sum(s.get("gap_count", 0) for s in streams)
    out["max_idr_kb"] = max((s.get("max_idr_bytes", 0) for s in streams), default=0) / 1024.0
    out["shared_windows"] = len(report.get("shared_loss_windows", []))
    out["streams"] = len(streams)

    # длительность — по клиентам (ближе к реальности, чем по pcap)
    elapsed = []
    clients = []
    for name in sorted(os.listdir(directory)):
        if not name.startswith("client-") or not name.endswith(".json"):
            continue
        data = load_json(os.path.join(directory, name))
        if not data:
            continue
        clients.append(data)
        if data.get("elapsed_s"):
            elapsed.append(data["elapsed_s"])
    out["elapsed_s"] = max(elapsed) if elapsed else 0.0
    out["idr_per_s"] = out["idr_frames"] / out["elapsed_s"] if out["elapsed_s"] else 0.0
    out["client_lost"] = sum(c.get("lost_frames", 0) for c in clients)
    out["client_frames"] = sum(c.get("frames", 0) for c in clients)
    out["terminated"] = [c.get("termination_code") for c in clients if c.get("terminated")]
    out["poor_status"] = sum(c.get("poor_status", 0) for c in clients)

    # запросы IDR от wolf
    try:
        with open(os.path.join(directory, "wolf.log"), errors="replace") as f:
            out["forcing_idr"] = f.read().count("Forcing IDR")
    except OSError:
        out["forcing_idr"] = 0

    return out


def fill_ticket(path, runs):
    """Подставляет значения в таблицу TICKET.md вместо плейсхолдеров."""
    try:
        with open(path) as f:
            text = f.read()
    except OSError:
        print(f"тикет не найден: {path}", file=sys.stderr)
        return False

    values = []
    for r in runs:
        if r is None:
            values += ["н/д", "н/д", "н/д"]
            continue
        values += [
            "{:.0f}".format(r["lost_frames"]),
            "{:.2f}".format(r["idr_per_s"]),
            "{:.0f}".format(r["shared_windows"]),
        ]

    it = iter(values)
    filled = re.sub(r"`<\.\.\.>`", lambda m: "`{}`".format(next(it)), text)
    with open(path, "w") as f:
        f.write(filled)
    print(f"тикет заполнен: {path}")
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifacts", required=True, help="каталог с прогонами")
    parser.add_argument("--ticket", default=None, help="куда подставить значения")
    args = parser.parse_args()

    runs = []
    for name, label in RUNS:
        directory = os.path.join(args.artifacts, name)
        if os.path.isdir(directory):
            runs.append(read_run(directory))
        else:
            print(f"прогон {name}: нет каталога {directory}", file=sys.stderr)
            runs.append(None)

    header = (
        "| прогон | потеряно кадров (pcap) | I-кадров/с | общих окон потерь | "
        "Forcing IDR в логе | кадров принято клиентами | разрыв сессии |"
    )
    sep = "|---|---|---|---|---|---|---|"
    rows = []
    for (name, label), r in zip(RUNS, runs):
        if r is None:
            rows.append(f"| {label} | н/д | н/д | н/д | н/д | н/д | н/д |")
            continue
        term = ", ".join(str(c) for c in r["terminated"]) if r["terminated"] else "нет"
        rows.append(
            f"| {label} | {r['lost_frames']} | {r['idr_per_s']:.2f} | {r['shared_windows']} | "
            f"{r['forcing_idr']} | {r['client_frames']} | {term} |"
        )

    print()
    print(header)
    print(sep)
    for row in rows:
        print(row)
    print()

    for (name, _), r in zip(RUNS, runs):
        if r is None:
            continue
        print(
            f"{name}: потоков {r['streams']}, пропусков {r['gaps']}, "
            f"макс. I-кадр {r['max_idr_kb']:.1f} КБ, "
            f"потери по клиентам {r['client_lost']}, статус POOR {r['poor_status']}"
        )

    if args.ticket:
        fill_ticket(args.ticket, runs)

    return 0


if __name__ == "__main__":
    sys.exit(main())
