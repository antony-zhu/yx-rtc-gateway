import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("package_yxot", ROOT / "tools/package_yxot.py")
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.payload = self.directory / "firmware.bin"
        self.payload.write_bytes(b"\xe9" + bytes(range(256)) * 17)
        self.output = self.directory / "firmware.yxot"

    def test_layout_and_independent_hashes(self):
        metadata = PACKAGE.package(self.payload, self.output)
        payload = self.payload.read_bytes()
        container = self.output.read_bytes()
        self.assertEqual(container[:4], b"YXOT")
        self.assertEqual(struct.unpack("<I", container[4:8])[0], 1)
        self.assertEqual(container[8:40], hashlib.sha256(payload).digest())
        self.assertEqual(container[40:], payload)
        self.assertEqual(metadata["size"], len(payload) + 40)
        self.assertEqual(metadata["sha256"], hashlib.sha256(container).hexdigest())
        self.assertEqual(metadata["payload_sha256"], hashlib.sha256(payload).hexdigest())
        self.assertNotEqual(metadata["sha256"], metadata["payload_sha256"])

    def test_output_is_deterministic(self):
        first = PACKAGE.package(self.payload, self.output)
        content = self.output.read_bytes()
        second = PACKAGE.package(self.payload, self.output)
        self.assertEqual(first, second)
        self.assertEqual(content, self.output.read_bytes())

    def test_only_explicit_manifest_uses_container_metadata(self):
        manifest = self.directory / "latest.json"
        with contextlib.redirect_stdout(io.StringIO()):
            PACKAGE.main([str(self.payload), str(self.output)])
        self.assertFalse(manifest.exists())
        with contextlib.redirect_stdout(io.StringIO()):
            metadata = PACKAGE.main([str(self.payload), str(self.output), "--manifest", str(manifest),
                                     "--version", "release-2026", "--url", "https://host/fw.yxot"])
        self.assertEqual(json.loads(manifest.read_text(encoding="utf-8")), {
            "version": "release-2026", "url": "https://host/fw.yxot",
            "size": metadata["size"], "sha256": metadata["sha256"],
        })

    def test_empty_payload_and_same_path_do_not_overwrite_input(self):
        original = self.payload.read_bytes()
        with self.assertRaises(ValueError):
            PACKAGE.package(self.payload, self.payload)
        self.assertEqual(self.payload.read_bytes(), original)
        self.payload.write_bytes(b"")
        with self.assertRaises(ValueError):
            PACKAGE.package(self.payload, self.output)
        self.assertFalse(self.output.exists())

    def test_invalid_manifest_arguments_create_no_output(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                PACKAGE.main([str(self.payload), str(self.output), "--manifest", str(self.directory / "latest.json")])
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
