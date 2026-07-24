#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 qstrdefs.preprocessed.h。

官方规则依赖 `cat | sed | compiler | sed > file` 这一串 shell 管道，
在 Windows + ESP-IDF 的默认环境里既不稳定，也容易受命令长度影响。
这里改成纯 Python 流程，保持语义一致：
1. 拼接 qstrdefs 输入文件。
2. 把 `Q(...)` 行临时包成字符串，交给预处理器展开。
3. 再把 `"Q(...)"` 还原回 `Q(...)`。
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile


def read_items(path: str) -> list[str]:
    return [
        line.strip()
        for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]


def write_rsp_file(args: list[str]) -> str:
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", suffix=".rsp", delete=False
    ) as fp:
        for arg in args:
            fp.write(arg.replace("\\", "/"))
            fp.write("\n")
        return fp.name


def build_combined_input(input_files: list[str]) -> str:
    parts: list[str] = []
    for input_path in input_files:
        content = pathlib.Path(input_path).read_text(encoding="utf-8")
        transformed_lines: list[str] = []
        for line in content.splitlines():
            if re.match(r"^Q\(.*\)$", line):
                transformed_lines.append(f"\"{line}\"")
            else:
                transformed_lines.append(line)
        parts.append("\n".join(transformed_lines))
    return "\n".join(parts) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate qstrdefs.preprocessed.h")
    parser.add_argument("--compiler", required=True, help="C compiler path")
    parser.add_argument("--flags-file", required=True, help="Preprocessor flags file")
    parser.add_argument("--qstrdefs", required=True, help="Core qstrdefs.h path")
    parser.add_argument("--ports-file", required=True, help="Port qstrdefs file list")
    parser.add_argument("--collected", required=True, help="Collected qstr path")
    parser.add_argument("--output", required=True, help="Output header path")
    args = parser.parse_args()

    flags = read_items(args.flags_file)
    port_files = read_items(args.ports_file)
    input_files = [args.qstrdefs, *port_files, args.collected]

    combined_input = build_combined_input(input_files)
    output_path = pathlib.Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    temp_input_path = None
    rsp_path = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", suffix=".h", delete=False
        ) as fp:
            fp.write(combined_input)
            temp_input_path = fp.name.replace("\\", "/")

        rsp_path = write_rsp_file(["-E", *flags, temp_input_path])
        preprocessed = subprocess.check_output(
            [args.compiler, f"@{rsp_path}"], stderr=subprocess.STDOUT, text=True
        )

        restored_lines: list[str] = []
        for line in preprocessed.splitlines():
            match = re.fullmatch(r'"(Q\(.*\))"', line)
            if match is not None:
                restored_lines.append(match.group(1))
            else:
                restored_lines.append(line)

        output_path.write_text("\n".join(restored_lines) + "\n", encoding="utf-8")
        return 0
    except subprocess.CalledProcessError as exc:
        sys.stdout.write(exc.output)
        return exc.returncode
    finally:
        for path in (rsp_path, temp_input_path):
            if path is None:
                continue
            try:
                os.unlink(path)
            except OSError:
                pass


if __name__ == "__main__":
    sys.exit(main())
