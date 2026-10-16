"""Run the production network manager and Ethernet driver with deterministic host mocks."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="gcc")
    args = parser.parse_args()
    source_dir = Path(__file__).resolve().parent
    environment = os.environ.copy()
    environment["PATH"] = str(Path(args.compiler).resolve().parent) + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="network-lifecycle-tests-") as directory:
        for name, source, target_4g in [
            ("net-manager-4g", "test_net_manager.c", 1),
            ("net-manager-ethernet", "test_net_manager.c", 0),
            ("ethernet-driver", "test_ethernet.c", 0),
        ]:
            output = Path(directory) / (name + ".exe")
            command = [args.compiler, "-std=gnu11", "-O0", "-g",
                       "-Werror=implicit-function-declaration",
                       f"-DGW_NETWORK_TARGET_4G={target_4g}",
                       f"-DGW_NETWORK_TARGET_ETHERNET={1 - target_4g}",
                       "-I", str(source_dir / "stubs"),
                       "-I", str(ROOT / "embedded/components/gw_app/include"),
                       "-I", str(ROOT / "embedded/components/gw_drivers/include"),
                       str(source_dir / source), "-o", str(output)]
            subprocess.run(command, cwd=ROOT, env=environment, check=True)
            subprocess.run([str(output)], cwd=ROOT, env=environment, check=True)


if __name__ == "__main__":
    main()
