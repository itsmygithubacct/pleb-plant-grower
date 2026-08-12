#!/usr/bin/env python3
"""Expand ART_BIBLE.md's prompt template into assets/graphics/prompts.json.

The bible is the authority on every prompt, and this reads it rather than
restating it. A transcribed copy of 24 prompt lines would drift from the
document that governs them within about a week, and the drift would be
invisible: both would look plausible.

So the template, the per-species phrase and the 24 per-state deltas are all
parsed out of ART_BIBLE.md. Change the bible, re-run this, and the ledger
follows. If the bible's shape changes enough that parsing fails, this refuses
loudly rather than emitting a half-expanded prompt.

The ledger it writes is what the generator reads and what ships as provenance:
every generation is recorded verbatim, with no hostname, path or name in it.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

BIBLE_DEFAULT = pathlib.Path(
    "../../../research/gpu_terminal/games/pleb-plant-grower/ART_BIBLE.md")

# Magenta keys everything except the calathea, whose deep purple undersides
# would be keyed out with it -- so she gets cyan. That distinction is the
# bible's and it is the sort of thing a transcription silently loses.
KEYS = {
    "pothos": ("#ff00ff", "magenta, pink, or purple"),
    "snake-plant": ("#ff00ff", "magenta, pink, or purple"),
    "peace-lily": ("#ff00ff", "magenta, pink, or purple"),
    "calathea": ("#00ffff", "cyan, turquoise, teal, or aqua"),
}

HEALTH_ROWS = ("thriving", "healthy", "thirsty", "distressed", "critical",
               "dead")


class RecipeError(RuntimeError):
    pass


def parse_bible(text: str) -> tuple[str, str, dict[str, str], dict[str, str]]:
    """Return (style preamble, template, plant_common, state by file id).

    The bible says the preamble is "stored once as STYLE_PREAMBLE in
    tools/art_recipes.py". It is parsed out of the bible instead, which honours
    the intent better than a constant would: stored once AND never able to
    disagree with the document that governs it.
    """
    preamble = None
    for block in re.findall(r"```text\n(.*?)```", text, re.S):
        if block.lstrip().startswith("Use case:"):
            preamble = block.strip("\n")
            break
    if preamble is None:
        raise RecipeError(
            "no style preamble found: expected a ```text block starting "
            "'Use case:'")

    template = None
    for block in re.findall(r"```text\n(.*?)```", text, re.S):
        if "{PLANT_COMMON}" in block and "{STATE}" in block:
            template = block.strip("\n")
            break
    if template is None:
        raise RecipeError(
            "no prompt template found: expected a ```text block containing "
            "{PLANT_COMMON} and {STATE}")

    common: dict[str, str] = {}
    for label, phrase in re.findall(
            r"^\|\s*(pothos|snake plant|peace lily|calathea)\s*\|\s*`([^`]+)`\s*\|",
            text, re.M):
        common[label.replace(" ", "-")] = phrase

    states: dict[str, str] = {}
    for file_id, state in re.findall(
            r"^\|\s*`((?:pothos|snake-plant|peace-lily|calathea)-[a-z]+)`\s*\|\s*([^|]+?)\s*\|",
            text, re.M):
        states[file_id] = state

    missing_common = sorted(set(KEYS) - set(common))
    if missing_common:
        raise RecipeError(f"no PLANT_COMMON for: {', '.join(missing_common)}")

    expected = {f"{species}-{row}" for species in KEYS for row in HEALTH_ROWS}
    missing_states = sorted(expected - set(states))
    if missing_states:
        raise RecipeError(
            f"{len(missing_states)} state lines missing from the bible: "
            f"{', '.join(missing_states[:4])}"
            + (" ..." if len(missing_states) > 4 else ""))
    return preamble, template, common, states


# The other thirteen generations of ART_BIBLE §4.8. The plant strips are 24 of
# 37 and it is easy to stop there, because they are the ones with a table of
# their own; the pots, tools, decals, six backgrounds and title are single
# blocks further down the same section and are just as required.
SHEET_BLOCKS = (
    ("pots", "a sixteen-cell reference sheet of empty plant pots",
     "source/pots-chroma.png", "1254x1254"),
    ("tools", "a sixteen-cell icon sheet of houseplant care tools",
     "source/tools-chroma.png", "1254x1254"),
    ("decals", "a sixteen-cell sheet of small isolated plant-damage",
     "source/decals-chroma.png", "1254x1254"),
    ("title", "full-screen 16:9 title illustration",
     "bitmaps/title.png", "1672x941"),
    ("spathe", "an eight-cell reference sheet of a single peace-lily flower",
     "source/spathe-chroma.png", "1254x1254"),
    ("vines", "a sixteen-cell tile sheet of trailing pothos vine segments",
     "source/vines-chroma.png", "1254x1254"),
)

SCENES = (
    ("sunny-sill", "wide south-facing window sill, strong directional sun"),
    ("bright-corner", "bright corner a metre from a window, no direct sun"),
    ("study-desk", "a desk against a wall with a lamp, indirect daylight"),
    ("plain-studio", "a plain studio wall and shelf, even soft light"),
    ("kitchen-shelf", "a kitchen shelf near a window, warm and busy"),
    ("steamy-bath", "a small bathroom shelf, humid, soft diffused light"),
)


def parse_sheet_blocks(text: str) -> dict[str, str]:
    """The non-plant ```text blocks, keyed by the asset they describe."""
    found: dict[str, str] = {}
    for block in re.findall(r"```text\n(.*?)```", text, re.S):
        if "{PLANT_COMMON}" in block or block.lstrip().startswith("Use case:"):
            continue
        body = block.strip("\n")
        for key, needle, _master, _canvas in SHEET_BLOCKS:
            if needle in body:
                found[key] = body
        if "{SCENE}" in body:
            found["background"] = body
    return found


def build_special_entries(preamble: str, template: str,
                          common: dict[str, str], text: str) -> list[dict]:
    """calathea-night, which the bible defines as the plant template with one
    substituted {STATE} rather than as a block of its own.

    The other two specials -- spathe and vines -- are named in §4.2 with grid,
    cell and source path, and have NO prompt anywhere in the bible. They are
    reported rather than invented: guessing a prompt for a peace-lily flower
    sheet would produce art that looks authored and answers to nothing.
    """
    entries = []
    marker = "**`calathea-night`**"
    start = text.find(marker)
    match = None
    if start >= 0:
        tail = text[start:text.find("###", start)]
        # the state is the italic run after "{STATE} ="
        # The bible writes it as: `{STATE}` = \n*"...text..."*
        # An earlier pattern here anchored on {STATE} without its backticks and
        # silently matched nothing, so calathea-night was quietly absent from a
        # ledger that otherwise looked complete.
        quoted = re.search(r'\*"(.+?)"\*', tail, re.S)
        if quoted:
            match = quoted
    if match:
        state = " ".join(match.group(1).split())
        prompt = (template
                  .replace("<STYLE_PREAMBLE>", preamble)
                  .replace("{PLANT_COMMON}", common["calathea"])
                  .replace("{STATE}", state)
                  .replace("{KEY}", "#00ffff")
                  .replace("{NOKEY}", "cyan, turquoise, teal, or aqua"))
        leftover = re.search(r"\{[A-Z_]+\}|<[A-Z_]+>", prompt)
        if leftover:
            raise RecipeError(f"calathea-night: unexpanded "
                              f"{leftover.group(0)}")
        entries.append({"id": "calathea-night", "kind": "plant-growth-strip",
                        "species": "calathea", "health": "night", "cells": 4,
                        "chroma_key": "#00ffff",
                        "master": "source/calathea-night-chroma.png",
                        "canvas": "1672x941", "prompt": prompt})
    return entries


def build_sheet_entries(preamble: str, blocks: dict[str, str]) -> list[dict]:
    entries: list[dict[str, object]] = []
    for key, _needle, master, canvas in SHEET_BLOCKS:
        if key not in blocks:
            raise RecipeError(f"no prompt block found for {key}")
        prompt = blocks[key].replace("<STYLE_PREAMBLE>", preamble)
        leftover = re.search(r"\{[A-Z_]+\}|<[A-Z_]+>", prompt)
        if leftover:
            raise RecipeError(f"{key}: unexpanded {leftover.group(0)}")
        entries.append({"id": key, "kind": "sheet", "master": master,
                        "canvas": canvas, "prompt": prompt})
    if "background" not in blocks:
        raise RecipeError("no background prompt block found")
    for scene, summary in SCENES:
        prompt = (blocks["background"]
                  .replace("<STYLE_PREAMBLE>", preamble)
                  .replace("{SCENE_SUMMARY}", summary)
                  .replace("{SCENE}", scene))
        leftover = re.search(r"\{[A-Z_]+\}|<[A-Z_]+>", prompt)
        if leftover:
            raise RecipeError(f"{scene}: unexpanded {leftover.group(0)}")
        entries.append({"id": f"background-{scene}", "kind": "background",
                        "scene": scene,
                        "master": f"backgrounds/{scene}/{scene}.png",
                        "canvas": "1672x941", "prompt": prompt})
    return entries


def build_entries(preamble: str, template: str, common: dict[str, str],
                  states: dict[str, str]) -> list[dict[str, object]]:
    entries = []
    for species in KEYS:
        key, nokey = KEYS[species]
        for row in HEALTH_ROWS:
            file_id = f"{species}-{row}"
            prompt = (template
                      .replace("<STYLE_PREAMBLE>", preamble)
                      .replace("{PLANT_COMMON}", common[species])
                      .replace("{STATE}", states[file_id])
                      .replace("{KEY}", key)
                      .replace("{NOKEY}", nokey))
            # Both bracket styles: the template uses {NAME} for substitutions
            # and <NAME> for the shared preamble, and checking only one is how
            # a literal <STYLE_PREAMBLE> nearly shipped inside 24 prompts.
            leftover = re.search(r"\{[A-Z_]+\}|<[A-Z_]+>", prompt)
            if leftover:
                raise RecipeError(
                    f"{file_id}: unexpanded placeholder {leftover.group(0)} "
                    f"remains in the prompt")
            entries.append({
                "id": file_id,
                "species": species,
                "health": row,
                # The strip is one generation: one health row across all four
                # ages, so their relative sizes are generated together (D-056).
                "kind": "plant-growth-strip",
                "cells": 4,
                "chroma_key": key,
                "master": f"source/{file_id}-chroma.png",
                "canvas": "1672x941",
                "prompt": prompt,
            })
    return entries


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bible", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path,
                        default=pathlib.Path("assets/graphics/prompts.json"))
    parser.add_argument("--check", action="store_true",
                        help="re-expand and compare rather than write")
    args = parser.parse_args(argv[1:])

    root = pathlib.Path(__file__).resolve().parent.parent
    bible = args.bible or (root / BIBLE_DEFAULT).resolve()
    if not bible.is_file():
        print(f"art-recipes: cannot read the bible at {bible}",
              file=sys.stderr)
        return 1

    try:
        text = bible.read_text()
        preamble, template, common, states = parse_bible(text)
        entries = build_entries(preamble, template, common, states)
        entries += build_special_entries(preamble, template, common, text)
        entries += build_sheet_entries(preamble, parse_sheet_blocks(text))
    except RecipeError as error:
        print(f"art-recipes: {error}", file=sys.stderr)
        return 1

    ledger = {
        "schema_version": 1,
        "_purpose": ("Every generation, recorded verbatim. Expanded from "
                     "ART_BIBLE.md by tools/art_recipes.py -- edit the bible, "
                     "not this file."),
        "generator": "OpenAI built-in image generation via codex exec",
        "entries": entries,
    }
    out = root / args.out
    rendered = json.dumps(ledger, indent=2) + "\n"

    if args.check:
        if not out.is_file():
            print("art-recipes: prompts.json missing; run make art-recipes",
                  file=sys.stderr)
            return 1
        if out.read_text() != rendered:
            print("art-recipes: prompts.json is stale against ART_BIBLE.md",
                  file=sys.stderr)
            return 1
        print(f"art-recipes: PASS ({len(entries)} prompts match the bible)")
        return 0

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(rendered)
    print(f"art-recipes: wrote {len(entries)} prompts to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
