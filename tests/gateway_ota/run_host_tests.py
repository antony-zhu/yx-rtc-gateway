"""Compile and run the OTA manager against HTTP/flash/task mocks and real SHA-256."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="gcc")
    parser.add_argument("--idf", type=Path, default=os.environ.get("IDF_PATH"))
    args = parser.parse_args()
    if args.idf is None:
        parser.error("--idf or IDF_PATH must identify the local ESP-IDF tree")
    source_dir = Path(__file__).resolve().parent
    mbedtls = args.idf / "components/mbedtls/mbedtls"
    cjson = args.idf / "components/json/cJSON"
    environment = os.environ.copy()
    compiler_directory = str(Path(args.compiler).resolve().parent)
    environment["PATH"] = compiler_directory + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="gateway-ota-tests-") as directory:
        output = Path(directory) / "gateway-ota-tests.exe"
        command = [args.compiler, "-std=gnu11", "-O0", "-g",
                   '-DMBEDTLS_CONFIG_FILE="ota_mbedtls_config.h"',
                   "-I", str(source_dir / "stubs"),
                   "-I", str(ROOT / "embedded/components/gw_app/include"),
                   "-I", str(cjson), "-I", str(mbedtls / "include"),
                   str(source_dir / "test_ota.c"), str(cjson / "cJSON.c"),
                   str(mbedtls / "library/md.c"), str(mbedtls / "library/sha256.c"),
                   str(mbedtls / "library/platform_util.c"), str(mbedtls / "library/platform.c"),
                   "-lm", "-o", str(output)]
        subprocess.run(command, cwd=ROOT, env=environment, check=True)
        subprocess.run([str(output)], cwd=ROOT, env=environment, check=True)


if __name__ == "__main__":
    main()
