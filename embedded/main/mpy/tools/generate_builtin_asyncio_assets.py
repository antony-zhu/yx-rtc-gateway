#!/usr/bin/env python3
"""
根据上游 MicroPython asyncio 源文件生成 cat 内置补种头文件。

生成结果会把官方 `uasyncio.py` 和 `asyncio/` 包里的 Python 文本
转成 C 字符串常量，便于设备首次启动时自动补齐到 LittleFS。
"""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
MICROPY_ASYNCIO_DIR = ROOT / "third_party" / "micropython" / "extmod" / "asyncio"
OUTPUT = ROOT / "main" / "mpy" / "mpy_builtin_asyncio_assets.h"

ASSETS = [
    ("uasyncio.py", "/lib/uasyncio.py"),
    ("__init__.py", "/lib/asyncio/__init__.py"),
    ("core.py", "/lib/asyncio/core.py"),
    ("event.py", "/lib/asyncio/event.py"),
    ("funcs.py", "/lib/asyncio/funcs.py"),
    ("lock.py", "/lib/asyncio/lock.py"),
    ("stream.py", "/lib/asyncio/stream.py"),
    ("task.py", "/lib/asyncio/task.py"),
]


def sanitize_name(name: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in name)


def to_c_string(text: str) -> str:
    lines: list[str] = []
    if text == "":
        return '""'

    for line in text.splitlines(keepends=True):
        escaped = (
            line.replace("\\", "\\\\")
            .replace('"', '\\"')
            .replace("\t", "\\t")
            .replace("\r", "\\r")
            .replace("\n", "\\n")
        )
        lines.append(f'"{escaped}"')
    return "\n".join(lines)


def main() -> None:
    rendered_assets: list[str] = []
    rendered_table: list[str] = []

    for source_name, logical_path in ASSETS:
        source_path = MICROPY_ASYNCIO_DIR / source_name
        text = source_path.read_text(encoding="utf-8")
        symbol = sanitize_name(logical_path.strip("/"))
        rendered_assets.append(
            f"static const char s_cat_mpy_builtin_{symbol}[] =\n{to_c_string(text)};\n"
        )
        rendered_table.append(
            f'    {{"{logical_path}", s_cat_mpy_builtin_{symbol}}},'
        )

    content = [
        "#pragma once",
        "",
        "#include <stddef.h>",
        "",
        "/*",
        " * 本文件由 tools/generate_builtin_asyncio_assets.py 自动生成。",
        " * 如需更新 asyncio/uasyncio 内置补种资源，请重新运行该脚本。",
        " */",
        "",
        "typedef struct {",
        "    const char *path;",
        "    const char *text;",
        "} cat_mpy_builtin_text_asset_t;",
        "",
        *rendered_assets,
        "static const cat_mpy_builtin_text_asset_t s_cat_mpy_builtin_asyncio_assets[] = {",
        *rendered_table,
        "};",
        "",
        "static const size_t s_cat_mpy_builtin_asyncio_asset_count =",
        "    sizeof(s_cat_mpy_builtin_asyncio_assets) / sizeof(s_cat_mpy_builtin_asyncio_assets[0]);",
        "",
    ]

    OUTPUT.write_text("\n".join(content), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
