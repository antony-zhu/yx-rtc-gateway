#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""运行一个 Python 脚本，并把标准输出稳定写入目标文件。"""

from __future__ import annotations

import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print(
            "usage: micropython_run_to_file.py <script> <output> [args ...]",
            file=sys.stderr,
        )
        return 2

    script = sys.argv[1]
    output = sys.argv[2]
    script_args = sys.argv[3:]

    try:
        result = subprocess.check_output(
            [sys.executable, script, *script_args], stderr=subprocess.STDOUT
        )
    except subprocess.CalledProcessError as exc:
        sys.stdout.buffer.write(exc.output)
        return exc.returncode

    pathlib.Path(output).write_bytes(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
