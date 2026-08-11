#!/usr/bin/env python3
"""Art review gate.

Every atlas and bitmap named in assets/graphics/manifest.json must have an
entry in docs/art-review.json whose sha256 matches the committed file and whose
verdict is 'accept'. Generated art is never committed on an agent's say-so.

Passes trivially on an empty manifest, which is why it exists from Milestone 0.
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "assets" / "graphics" / "manifest.json"
REVIEW = ROOT / "docs" / "art-review.json"


def load(path: Path) -> dict:
    if not path.exists() or path.stat().st_size == 0:
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        print(f"art-review: {path.name} is not valid JSON: {exc}",
              file=sys.stderr)
        raise SystemExit(1)


def main() -> int:
    manifest = load(MANIFEST)
    entries = manifest.get("entries", []) if manifest else []
    if not entries:
        print("test-art-review: PASS (no art yet)")
        return 0

    review = load(REVIEW)
    reviewed = {e.get("path"): e for e in review.get("entries", [])}
    failures = []

    for item in entries:
        rel = item.get("path")
        if rel is None:
            failures.append("manifest entry without a path")
            continue
        record = reviewed.get(rel)
        if record is None:
            failures.append(f"{rel}: no review record")
            continue
        if record.get("verdict") != "accept":
            failures.append(f"{rel}: verdict is {record.get('verdict')!r}")
            continue
        blob = ROOT / rel
        if not blob.exists():
            failures.append(f"{rel}: named in the manifest but not on disk")
            continue
        digest = hashlib.sha256(blob.read_bytes()).hexdigest()
        if digest != record.get("sha256"):
            failures.append(f"{rel}: sha256 does not match the reviewed file")

    if failures:
        for f in failures:
            print(f"art-review: {f}", file=sys.stderr)
        return 1
    print(f"test-art-review: PASS ({len(entries)} reviewed)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
