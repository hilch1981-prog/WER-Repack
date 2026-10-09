"""Keeper regressions for the read-only publication verifier; no game stack required."""

import contextlib
import hashlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "wer_manifest_verifier", Path(__file__).resolve().parents[1] / "verify-source-manifest.py"
)
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


class ManifestVerification(unittest.TestCase):
    def setUp(self):
        self.fixture = tempfile.TemporaryDirectory(prefix="wer_manifest_test_")
        self.addCleanup(self.fixture.cleanup)
        self.repo = Path(self.fixture.name)
        self.root = self.repo / "source" / "WER_Source"
        self.root.mkdir(parents=True)
        (self.root / "example.cpp").write_bytes(b"// fixture\n")
        self.rows = [{"path": "example.cpp", "size": 11,
                      "sha256": hashlib.sha256(b"// fixture\n").hexdigest()}]

    def check(self):
        (self.repo / "source" / "SOURCE_MANIFEST.json").write_text(
            json.dumps(self.rows), encoding="utf-8"
        )
        with contextlib.redirect_stdout(io.StringIO()):
            return verifier.verify(self.repo)

    def test_valid(self):
        self.assertEqual(self.check(), 0)

    def test_missing(self):
        (self.root / "example.cpp").unlink()
        self.assertEqual(self.check(), 1)

    def test_hash_mismatch(self):
        (self.root / "example.cpp").write_bytes(b"// changed\n")
        self.assertEqual(self.check(), 1)

    def test_size_mismatch(self):
        self.rows[0]["size"] += 1
        self.assertEqual(self.check(), 1)

    def test_unlisted(self):
        (self.root / "extra.cpp").write_bytes(b"fixture")
        self.assertEqual(self.check(), 1)

    def test_duplicate(self):
        self.rows.append(dict(self.rows[0]))
        self.assertEqual(self.check(), 1)

    def test_unsafe_paths(self):
        for unsafe in ("../outside", "/outside", "C:/outside", "folder\\outside"):
            with self.subTest(path=unsafe):
                self.rows[0]["path"] = unsafe
                self.assertEqual(self.check(), 1)


if __name__ == "__main__":
    unittest.main()
