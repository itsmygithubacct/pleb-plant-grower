#!/usr/bin/env python3
"""Embed guard.

kilix-land links libpleb-plant-grower.a into its own process. The archive must
therefore never pull in a terminal session, an event loop, or an exit path: the
host owns all three. This greps the archive's undefined symbols for the ones
that would prove otherwise.
"""
from __future__ import annotations

import subprocess
import sys

FORBIDDEN = (
    "kittyts_start", "kittyts_stop",
    "kittyfb_present", "kittyin_poll",
    "kilix_game_host_run",
    "exit", "_exit", "abort",
    "tcsetattr", "tcgetattr",
)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: check_embed_guard.py <nm> <archive>", file=sys.stderr)
        return 2
    nm, archive = argv[1], argv[2]
    proc = subprocess.run([nm, "--undefined-only", archive],
                          capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        print(f"embed-guard: {nm} failed on {archive}", file=sys.stderr)
        return 1
    undefined = {
        line.split()[-1]
        for line in proc.stdout.splitlines()
        if line.strip() and not line.rstrip().endswith(":")
    }
    hits = sorted(s for s in FORBIDDEN if s in undefined)
    if hits:
        print("embed-guard: archive references host-owned symbols: "
              + ", ".join(hits), file=sys.stderr)
        return 1
    print("embed-guard: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
