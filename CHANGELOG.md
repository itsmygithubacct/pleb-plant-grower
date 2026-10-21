# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/) and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## Unreleased

### Added

- The simulation: a 900 s care tick, a derived watering interval, nine care
  axes, the symptom ladders, and a catch-up engine that credits up to 30 days
  and resolves anything longer as abandonment.
- Transactional persistence with an A/B generation pair, so a crash mid-write
  costs the newest record and never the plant.
- A renderer with a ten-layer stack, a procedural stage for background-off, and
  115 render fixtures with frozen goldens.
- Seven screens, a calendar, a journal, and meters that read in words rather
  than numbers.
- An instruction surface that names competing causes and the observation that
  separates them, checked against a myth blocklist.
- A standalone terminal frontend on the kilix game host.
- A twenty-cue SFX bank generated with integer arithmetic only, byte
  reproducible, with per-cue provenance.
- Three procedural atlases authored at final resolution.
- `docs/EMBEDDING.md` and a tested archive-only embed contract.

### Fixed

- The calendar numbered its cells from the first grid cell, not from the
  first of the month, and at 2x and above its numbers drifted out of their
  cells. Both were kilix-ui bugs, fixed in kilix-ui 0.4.1. The SDK pin moves
  to that fix alone, so pleb-plant-grower and kilix-land still build from one
  SDK commit.
- `pg_graphics_shutdown` released the plant atlases and plates but not the
  four overlay sheets, so a host that zeroed its record afterwards leaked
  them. The embed test now checks every image is released, and
  `make sanitize` covers the render and embed tests as well.

### Not yet included

- The 37 model art generations. They are held behind the owner review gate
  (`make test-art-review`), which is the project's only deliberate hold.
- The kilix-land conservatory, pending that repository's own SDK pointer.
