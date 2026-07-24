#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Windows 友好的 qstr 预处理驱动。

这个脚本用于替代官方 `makeqstrdefs.py pp` 的长命令行调用方式。
核心目的有两个：
1. 避免 `cmd.exe` 8191 字符限制导致的构建失败。
2. 通过 GCC response file 传递参数，减少 Windows 下的命令拼接问题。
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Iterable


def read_items(path: str) -> list[str]:
    """按行读取参数文件，忽略空行。"""
    return [
        line.strip()
        for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]


def chunked(items: list[str], chunk_size: int) -> Iterable[list[str]]:
    """将源文件列表分块，避免单次预处理参数过大。"""
    for index in range(0, len(items), chunk_size):
        yield items[index : index + chunk_size]


def write_rsp_file(args: list[str]) -> str:
    """生成临时 response file，并返回其路径。"""
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", suffix=".rsp", delete=False
    ) as fp:
        for arg in args:
            fp.write(arg.replace("\\", "/"))
            fp.write("\n")
        return fp.name


def run_preprocess(
    compiler: str,
    flags: list[str],
    sources: list[str],
    out_fp,
    chunk_size: int,
) -> None:
    """逐块调用编译器预处理源文件，并把结果顺序写入输出文件。"""
    if not sources:
        return

    for chunk in chunked(sources, chunk_size):
        rsp_path = write_rsp_file(["-E", *flags, *chunk])
        try:
            output = subprocess.check_output(
                [compiler, f"@{rsp_path}"], stderr=subprocess.STDOUT
            )
        except subprocess.CalledProcessError as exc:
            sys.stdout.buffer.write(exc.output)
            sys.stdout.write(f"\n[qstr_pp] failed rsp: {rsp_path}\n")
            sys.stdout.flush()
            raise
        finally:
            if os.environ.get("CAT_KEEP_RSP") != "1":
                try:
                    os.unlink(rsp_path)
                except OSError:
                    pass
        out_fp.write(output)


def is_c_source(path: str) -> bool:
    return pathlib.Path(path).suffix.lower() == ".c"


def is_cxx_source(path: str) -> bool:
    return pathlib.Path(path).suffix in {
        ".cc",
        ".cp",
        ".cxx",
        ".cpp",
        ".CPP",
        ".c++",
        ".C",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate qstr.i.last on Windows")
    parser.add_argument("--compiler", required=True, help="C compiler path")
    parser.add_argument("--cflags-file", required=True, help="C flags list file")
    parser.add_argument("--cxxflags-file", required=True, help="CXX flags list file")
    parser.add_argument("--sources-file", required=True, help="QSTR source list file")
    parser.add_argument("--output", required=True, help="Output qstr.i.last path")
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=12,
        help="Number of source files per preprocess chunk",
    )
    args = parser.parse_args()

    cflags = read_items(args.cflags_file)
    cxxflags = read_items(args.cxxflags_file)
    sources = read_items(args.sources_file)

    csources = [source for source in sources if is_c_source(source)]
    cxxsources = [source for source in sources if is_cxx_source(source)]

    output_path = pathlib.Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with output_path.open("wb") as out_fp:
        run_preprocess(args.compiler, cflags, csources, out_fp, args.chunk_size)
        run_preprocess(args.compiler, cxxflags, cxxsources, out_fp, args.chunk_size)

    return 0


if __name__ == "__main__":
    sys.exit(main())
