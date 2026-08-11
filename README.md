# pleb-plant-grower

A realtime houseplant you actually have to look after. Plants grow on the wall
clock, not on a turn counter: close the game for a week and you come back to a
week's worth of consequences.

Runs two ways from one archive — standalone in a terminal, or embedded in
kilix-land. In standalone the backdrop is a scene you can swap or switch off
entirely, so the game can be a window on a shelf or a plant on a plain field.

## Status

Milestone 0. Repository skeleton, dependency pins, build, and the version gate.
There is no terminal frontend yet.

## Build

```sh
make
./pleb-plant-grower --version
make test
```

The build needs a C11 compiler, Make, and Python 3 for the content and asset
gates. The two pinned dependencies are `kilix-game-sdk` and `chip-sequencer`,
carried as submodules under `third_party/`.

## What is here

| Path | Contents |
|---|---|
| `include/pleb_plant_grower.h` | the one public header; the whole embed surface |
| `src/` | the game; `main.c` and `pg_term.c` are standalone-only |
| `mk/pleb-plant-grower.mk` | the fragment kilix-land includes |
| `content/` | plants, pots, spots, seasons — data, not code |
| `assets/` | graphics, backdrops, sound |
| `tools/` | content compiler, art and sound generation, validators |

## Design

Care numbers come from real houseplant schedules rather than invented ones, so
the four plants have genuinely different rhythms. Growth, health and recovery
are simulated continuously and shown on the plant itself: the game tells you
how it is doing by looking unwell, not by printing a number.

## License

MIT. See `LICENSE`.
