#!/usr/bin/env python3
"""Сверяет конфиг wolf на машине с шаблоном из репозитория.

wolf грузит пользовательский TOML целиком (`rfl::toml::load<WolfConfig,
rfl::DefaultIfMissing>`) — встроенный шаблон заново не подмешивается.
Поэтому расхождение с шаблоном означает «эта настройка реально работает
на машине», а не «это отличие от дефолта, которое можно игнорировать».

Использование:
    compare_config.py <host.toml> <template.toml> [--section gstreamer] [--keys]
"""
import argparse
import sys

try:
    import tomllib
except ImportError:  # python < 3.11
    sys.exit("нужен python 3.11+ (tomllib)")

# Ключи, которые по определению зависят от машины.
MACHINE_SPECIFIC = {
    "paired_clients",
    "profiles",
    "apps",
    "client_cert",
    "host_cert",
    "key_file",
}


def flatten(data, prefix=""):
    """Плоский словарь 'секция.ключ' -> значение (dict рекурсивно)."""
    out = {}
    for key, value in data.items():
        path = f"{prefix}{key}"
        if isinstance(value, dict):
            out.update(flatten(value, path + "."))
        else:
            out[path] = value
    return out


def render(value):
    text = repr(value)
    if len(text) > 120:
        text = text[:117] + "..."
    return text


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("host", help="конфиг с машины")
    parser.add_argument("template", help="шаблон из репозитория")
    parser.add_argument(
        "--section",
        action="append",
        default=None,
        help="сравнить только эту секцию (можно указать несколько раз)",
    )
    parser.add_argument("--keys", action="store_true", help="показать ключи, совпавшие с шаблоном")
    args = parser.parse_args()

    with open(args.host, "rb") as f:
        host = tomllib.load(f)
    with open(args.template, "rb") as f:
        template = tomllib.load(f)

    host_flat = flatten(host)
    template_flat = flatten(template)

    sections = args.section
    if sections:
        def keep(path):
            return any(path == s or path.startswith(s + ".") for s in sections)
    else:
        def keep(_path):
            return True

    only_host = {k: v for k, v in host_flat.items() if keep(k)}
    only_template = {k: v for k, v in template_flat.items() if keep(k)}

    def is_machine_specific(key):
        return any(part in MACHINE_SPECIFIC for part in key.split("."))

    same = []
    diff = []
    for key, value in sorted(only_host.items()):
        if key not in only_template:
            continue
        if only_template[key] == value:
            same.append(key)
        else:
            diff.append((key, only_template[key], value))

    print(f"хост:    {args.host}")
    print(f"шаблон:  {args.template}")
    if sections:
        print(f"секции:  {', '.join(sections)}")
    print()
    print(f"совпало с шаблоном:  {len(same)}")
    print(f"отличается:          {len(diff)}")
    print(f"есть только на хосте: {len(set(only_host) - set(only_template))}")
    print(f"есть только в шаблоне:{len(set(only_template) - set(only_host))}")
    print()

    if diff:
        print("=== РАСХОЖДЕНИЯ (шаблон -> хост) ===")
        for key, want, got in diff:
            print(f"  {key}")
            print(f"      шаблон: {render(want)}")
            print(f"      хост:   {render(got)}")
        print()

    if args.keys:
        print("=== СОВПАЛО ===")
        for key in same:
            print(f"  {key} = {render(only_host[key])}")
        print()

    extra_host = sorted(set(only_host) - set(only_template))
    extra_template = sorted(set(only_template) - set(only_host))
    if extra_host:
        print("=== есть только на хосте ===")
        for key in extra_host:
            mark = " (машинно-зависимое)" if is_machine_specific(key) else ""
            print(f"  {key} = {render(only_host[key])}{mark}")
        print()
    if extra_template:
        print("=== есть только в шаблоне (не задано на хосте -> дефолт из структуры) ===")
        for key in extra_template:
            print(f"  {key} = {render(only_template[key])}")
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
