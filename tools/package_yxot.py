#!/usr/bin/env python3
"""Package an ESP OTA payload into a deterministic YXOT version 1 container."""

import argparse
import hashlib
import json
from pathlib import Path
import struct


def package(payload_path: Path, output_path: Path) -> dict:
    if payload_path.resolve() == output_path.resolve():
        raise ValueError("payload and output must be different files")
    payload = payload_path.read_bytes()
    if not payload:
        raise ValueError("OTA payload must not be empty")
    payload_digest = hashlib.sha256(payload).digest()
    container = b"YXOT" + struct.pack("<I", 1) + payload_digest + payload
    output_path.write_bytes(container)
    return {
        "size": len(container),
        "sha256": hashlib.sha256(container).hexdigest(),
        "payload_sha256": payload_digest.hex(),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("payload", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--manifest", type=Path, help="write the requested latest.json manifest")
    parser.add_argument("--version")
    parser.add_argument("--url")
    args = parser.parse_args(argv)
    if args.manifest is not None:
        if not args.version or not args.url or not args.url.startswith(("http://", "https://")):
            parser.error("--manifest requires a nonempty --version and an HTTP(S) --url")
        if args.manifest.resolve() in (args.payload.resolve(), args.output.resolve()):
            parser.error("manifest must be a different file from the payload and container")
    elif args.version is not None or args.url is not None:
        parser.error("--version and --url require --manifest")
    try:
        metadata = package(args.payload, args.output)
        if args.manifest is not None:
            manifest = {
                "version": args.version,
                "url": args.url,
                "size": metadata["size"],
                "sha256": metadata["sha256"],
            }
            args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                                     encoding="utf-8", newline="\n")
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(json.dumps(metadata, sort_keys=True))
    return metadata


if __name__ == "__main__":
    main()
