# Embedding pleb-plant-grower

How another game runs this one inside its own process. Written for kilix-land,
which enters the conservatory from the top edge of its social room, but nothing
below is specific to it.

The whole surface is `include/pleb_plant_grower.h`. There is one archive,
`libpleb-plant-grower.a`, and it contains no terminal session, no event loop,
no clock and no exit path — the host owns all four.

---

## 1. The recipe

```make
KILIX_PLANT_DIR ?= third_party/pleb-plant-grower
include $(KILIX_PLANT_DIR)/mk/pleb-plant-grower.mk

CPPFLAGS += $(PG_CPPFLAGS)
LDLIBS   += $(PG_LIB)
```

The fragment builds `$(PG_LIB)` on demand and touches nothing else. It restores
your `.DEFAULT_GOAL`, so including it cannot change what a bare `make` builds —
that is a real hazard with shared fragments and the reason the guard is there.

```c
#include "pleb_plant_grower.h"
#include "pg_state.h"          /* only if you hold pg_state by value */

static pg_state plant;         /* zeroed before first use */

pg_init(&plant, seed);
pg_advance(&plant, host_now(), &report);   /* whenever wall time has moved */
pg_update(&plant, &input, step_seconds);   /* your 60 Hz step */
pg_render(renderer, &plant, &graphics, NULL);
```

---

## 2. `pg_state_size()`, and the rebuild-together rule

`pg_state` is a **concrete struct held by value**, the way kilix-land holds
`land_game`. No allocator is involved anywhere in this game, which is the
property that makes it embeddable at all.

The price is that you and this archive share an aggregate layout:

> **A host and this archive must be built from one commit.** A host compiled
> against a stale `pg_state` and linked against a newer
> `libpleb-plant-grower.a` will corrupt its own stack. This is not a
> theoretical hazard — it is the ordinary consequence of two translation units
> disagreeing about a struct, and it fails silently rather than at link time.

`pg_state_size()` is the cheap check, and it is a function rather than a
`sizeof` in a header precisely because the header is the thing that might be
stale:

```c
if (pg_state_size() != sizeof(pg_state)) { /* refuse to run */ }
```

`tests/test_embed.c` asserts it on our side. Assert it on yours too, at start,
once.

An embedder that only wants the **size** — supplying its own storage and never
touching a field — needs `include/` alone and no layout at all. `PG_CPPFLAGS`
exposes both `include/` and `src/` because the documented by-value embed is
impossible to compile without the second, which it was until 2026-08-11.

---

## 3. What the core promises

Enforced by `make embed-guard`, which `nm`s the archive:

| Promise | Why it matters to you |
|---|---|
| No `kittyts_*`, `kittyfb_*`, `kittyin_*`, `kittykb_*` | You started the session; we will not touch it |
| No `kilix_game_host_*`, `kilix_game_signals_*` | Your loop, your signal handlers |
| **No clock at all** | Time arrives only in the `pg_now` you fill |
| No `exit`, `abort` | We cannot end your process |
| No allocation on any `pg_render*` path | Checked by `tests/test_noalloc.c` |
| Render intersects and **restores** your clip | Your canvas comes back as you left it |
| Render never mutates the state | `memcmp` before and after, in the tests |

The clock rule is the one to trust most and the one most worth understanding.
Nothing in the archive reads the time; you sample it and hand it in. That is
what makes a 30-day absence reproducible, and it is why the same save advances
identically in your process and in ours.

---

## 4. Time is yours to supply

```c
static pg_now host_now(void)
{
    pg_now now = {0};
    struct timespec wall, boot;
    struct tm local;
    time_t seconds;

    clock_gettime(CLOCK_REALTIME, &wall);
    clock_gettime(CLOCK_BOOTTIME, &boot);
    now.wall_s = wall.tv_sec;
    now.boot_s = boot.tv_sec;
    read_boot_id(now.boot_id);            /* or leave it zeroed */

    seconds = (time_t)now.wall_s;
    localtime_r(&seconds, &local);
    now.tz_offset_minutes = local.tm_gmtoff / 60;
    return now;
}
```

Three things about this that are easy to get wrong:

- **`tz_offset_minutes` is a simulation input, not a display preference.** It
  decides local hour, and local hour decides the daylight window, the
  calathea's night fold and when midnight rolls. Derive it on **every** sample.
  Deriving it once at startup makes a long-running session wrong the moment DST
  flips.
- **`CLOCK_BOOTTIME`, not `CLOCK_MONOTONIC`.** Monotonic stops across suspend,
  so a closed laptop costs the plant that time. Boottime does not.
- **A zeroed `boot_id` is fine and is handled.** On a host with no readable
  boot id, sixteen zero bytes compare equal to themselves, so a genuine reboot
  looks like one continuous boot with a rewound boot clock. The policy detects
  exactly that and falls back to the wall gap (D-095). You do not need to work
  around it.

`pg_advance` is **idempotent**: calling it twice with the same `pg_now` credits
nothing the second time. Call it whenever you like — once a second is plenty.

---

## 5. Storage

`pg_store_open(&store, base_directory)` takes your data root and appends its
own `app_id` component, so two games sharing a root cannot collide. Pass `NULL`
to use the platform default. `PLEB_PLANT_CONFIG_HOME`, if set to an absolute
path, overrides both — which is what the tests use and what a host should offer
if it has its own profile system.

Saving is A/B: a write always goes to the generation that is not live, so a
crash mid-write costs the newest record and never the plant.

---

## 6. Audio, which is deliberately asymmetric

The core never touches a mixer. It pushes `pg_audio_event` records into a
bounded ring; you drain them with `pg_take_audio_events` and map `kind` onto
whatever vocabulary you already have. kilix-land has fourteen semantic events
and this game has seventeen kinds; that mapping is yours to make and there is
no correct answer we could bake in.

**Standalone installs a generator on the mixer; embedded, we install nothing.**
There is one generator seam per mixer and it belongs to the host. The
take-and-clear event API is identical in both modes, which is exactly why the
asymmetry costs nothing.

---

## 7. Graphics

`pg_graphics_init(&graphics, asset_root, error, sizeof error)` resolves plates
under your asset root. Passing `NULL` for the graphics argument to `pg_render`
is legal and gives the procedural stage — a designed look, not a degradation.
A plate of the wrong size is refused rather than scaled, and a missing plate is
not an error.

`pg_render` fits its own view to whatever renderer you hand it, snapping to the
largest integer scale in 2..4 that fits exactly so the frame is never
letterboxed. Below 960x540 it takes a fractional fit rather than refusing.
