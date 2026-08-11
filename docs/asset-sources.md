# Asset sources

Every shipped byte, where it came from, and what was done to it. Nothing here
names a machine, a person or a home directory: this file is written to be
published.

## Runtime files

| Runtime file | Source | Processing |
|---|---|---|
| `assets/sfx/*.wav` (20 cues) | `tools/gen_sfx.py`, authored here | Integer synthesis, no libm and no floating point in any signal path. PCM mono 16-bit 44100 Hz, which is the only format pcm-mixer accepts. Hashes in `docs/audio-provenance.json`; `make check-sfx` regenerates and compares. |
| `assets/graphics/atlases/ui-skin.png` | `tools/gen_art.py`, authored here | 8×4 cells of 8×8. Nine-slice frame for `kilix_ui_draw_panel`, sharing its eight-colour palette with the renderer so a cell and a drawn rectangle cannot drift. |
| `assets/graphics/atlases/glyphs.png` | `tools/gen_art.py` | 8×2 cells of 16×16, sixteen glyphs. |
| `assets/graphics/atlases/particles.png` | `tools/gen_art.py` | 8×2 cells of 32×32, sixteen particles. |
| `assets/graphics/atlases/*.png` (plants, pots, tools) | **Model generation — not yet produced** | See below. |
| `assets/backgrounds/<id>/<id>.png` | **Model generation — not yet produced** | 1920×1080 plates, refused rather than scaled if the size is wrong. |
| `content/*.json` | Authored here, from `HOUSEPLANT_CARE_RESEARCH.md` | Compiled to a header by `tools/compile_content.py`; every number is cited and range-checked by `tools/check_care_schedule.py`. |

## Why three atlases are authored rather than generated

`ART_BIBLE.md` §4.7. Model art at 8–16 px is mush: a nine-slice frame needs its
corners to be exactly the corners, and a 16 px glyph needs every pixel to carry
meaning. The second reason is drift — the UI skin shares its palette with
`src/pg_render.c`, and a generated atlas could not be kept in agreement with a
drawn rectangle.

## Model generations

Not in this release. When produced, each is recorded in
`assets/graphics/prompts.json` with the model, the verbatim prompt, the canvas
size and the date, and each must carry an `accept` verdict with a matching
sha256 in the review manifest before `make test-art-review` will pass. That
gate is the project's only hold, and it is deliberately not automatable: it
asks whether the art is good, which is not a property a checksum has an opinion
about.

## Renderer contract

Everything is composited by this game's own software rasteriser (`soft-raster`
via `kilix-top-down-engine`) into an RGBA framebuffer, and streamed to the
terminal through the Kitty graphics protocol. There is no SDL, no X11 and no
ncurses anywhere in the dependency graph.

## Licence

MIT, `LICENSE`. The generated audio and the procedural atlases are original
work of this repository, produced by the tools named above; both regenerate
byte-for-byte from tracked sources.
