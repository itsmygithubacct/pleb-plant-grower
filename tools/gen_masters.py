#!/usr/bin/env python3
"""Drive image generation from assets/graphics/prompts.json.

One generation is one health row: a four-cell strip of the same plant at four
ages, so their relative sizes are produced together (D-056). Never one cell,
never a whole atlas.

Generation runs through `codex exec`, which owns the image tool. Two things
about that are worth knowing before changing this:

  - **stdin must be closed.** `codex exec` reads stdin, and in a background
    process the pipe never closes, so it blocks forever printing "Reading
    additional input from stdin...". `</dev/null` is not optional.
  - **`-healthy` first.** Each species' healthy strip is the reference anchor
    for its other five rows, so the order below is deliberate and `--anchors`
    generates only those four.

The output directory is a caller-supplied staging path, never the working
tree: nothing generated may enter a commit until the owner has reviewed it,
which is the project's only deliberate hold.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys

HEALTH_ORDER = ("healthy", "thriving", "thirsty", "distressed", "critical",
                "dead")


def load_entries(root: pathlib.Path) -> list[dict]:
    ledger = root / "assets/graphics/prompts.json"
    if not ledger.is_file():
        raise SystemExit("gen-masters: no prompts.json; run "
                         "tools/art_recipes.py first")
    entries = json.loads(ledger.read_text()).get("entries", [])
    if not entries:
        raise SystemExit("gen-masters: prompts.json has no entries")
    # Healthy first within each species, because that strip is the reference
    # the rest are generated against. Entries that are not plant strips -- the
    # pot and tool sheets, the six backgrounds, the title -- have no species or
    # health at all, and an earlier version of this sort assumed they did and
    # crashed the moment the ledger grew past the plant rows.
    def order(entry: dict) -> tuple:
        kind = entry.get("kind", "")
        species = entry.get("species", "~")
        health = entry.get("health", "")
        rank = (HEALTH_ORDER.index(health) if health in HEALTH_ORDER
                else len(HEALTH_ORDER))
        # plant strips, then specials, then sheets and backgrounds
        group = 0 if kind == "plant-growth-strip" else 1
        return (group, species, rank, entry["id"])

    return sorted(entries, key=order)


def target_for(entry: dict, out: pathlib.Path) -> pathlib.Path:
    """Where this generation lands, taken from the ledger's own master path.

    It used to be `<id>-chroma.png` for everything, which is right for the
    plant strips and wrong for the rest: a background is not chroma-keyed and
    belongs at backgrounds/<scene>/<scene>.png, and the sheets have their own
    names. The ledger already says where each master goes -- ignoring that
    field and reconstructing a name is how the two drift apart.
    """
    return out / pathlib.PurePosixPath(str(entry["master"])).name


def generate(entry: dict, out: pathlib.Path, timeout: int) -> str:
    target = target_for(entry, out)
    if target.exists():
        return "skip"
    width, _, height = entry["canvas"].partition("x")
    instruction = (
        f"Generate one image and save it to {target}. "
        f"The canvas must be landscape, as close to {width} by {height} "
        f"pixels as the tool allows.\n\n{entry['prompt']}\n\n"
        f"Save the file, then print only the absolute path.")
    log = out / f"{entry['id']}.log"
    with log.open("w") as handle, open("/dev/null") as devnull:
        try:
            subprocess.run(
                ["codex", "exec", "--skip-git-repo-check",
                 "--sandbox", "workspace-write", instruction],
                stdin=devnull, stdout=handle, stderr=subprocess.STDOUT,
                timeout=timeout, check=False)
        except subprocess.TimeoutExpired:
            return "timeout"
    return "ok" if target.exists() else "FAIL"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", type=pathlib.Path,
                        help="staging directory, outside the repository")
    parser.add_argument("--anchors", action="store_true",
                        help="only the four -healthy reference strips")
    parser.add_argument("--only", help="one entry id")
    parser.add_argument("--timeout", type=int, default=900)
    args = parser.parse_args(argv[1:])

    root = pathlib.Path(__file__).resolve().parent.parent
    out = args.out.resolve()
    if out.is_relative_to(root):
        raise SystemExit(
            f"gen-masters: {out} is inside the repository. Generated art must "
            f"stage outside the tree until it has been reviewed.")
    out.mkdir(parents=True, exist_ok=True)

    entries = load_entries(root)
    if args.anchors:
        entries = [e for e in entries if e.get("health") == "healthy"]
    if args.only:
        entries = [e for e in entries if e["id"] == args.only]
        if not entries:
            raise SystemExit(f"gen-masters: no entry {args.only}")

    print(f"gen-masters: {len(entries)} strips -> {out}", flush=True)
    results: dict[str, int] = {}
    for index, entry in enumerate(entries, start=1):
        status = generate(entry, out, args.timeout)
        results[status] = results.get(status, 0) + 1
        print(f"  [{index}/{len(entries)}] {status:7} {entry['id']}",
              flush=True)
    summary = " ".join(f"{k}={v}" for k, v in sorted(results.items()))
    print(f"gen-masters: {summary}")
    return 0 if results.get("FAIL", 0) == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
