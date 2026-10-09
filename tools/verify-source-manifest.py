#!/usr/bin/env python3
"""Read-only verification of the published WER source snapshot (Python 3.9+)."""

import hashlib
import json
import sys
from pathlib import Path, PurePosixPath


def verify(repo):
    root = (repo / "source" / "WER_Source").resolve()
    manifest = json.loads((repo / "source" / "SOURCE_MANIFEST.json").read_text(encoding="utf-8"))
    expected = set()
    errors = []
    for row in manifest:
        relative = row["path"]
        parts = PurePosixPath(relative)
        if parts.is_absolute() or ".." in parts.parts or "\\" in relative or ":" in relative:
            errors.append("unsafe manifest path")
            continue
        target = (root / relative).resolve()
        if root not in target.parents or relative in expected:
            errors.append("escaped or duplicate manifest path")
            continue
        expected.add(relative)
        if not target.is_file():
            errors.append("missing: " + relative)
            continue
        content = target.read_bytes()
        if len(content) != row["size"] or hashlib.sha256(content).hexdigest() != row["sha256"].lower():
            errors.append("hash/size mismatch: " + relative)
    actual = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()}
    errors.extend("unlisted: " + p for p in sorted(actual - expected))
    for error in errors:
        print(error)
    print("{}: {} manifest files".format("FAIL" if errors else "PASS", len(expected)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(verify(Path(__file__).resolve().parent.parent))
