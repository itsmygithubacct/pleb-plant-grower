/*
 * The render path allocates nothing.
 *
 * ARCHITECTURE.md §2.2 rule 5. kilix-land calls pg_render inside its own frame
 * loop, in a process it owns the heap of; an allocation there is a latency
 * spike at best and a fragmentation problem over a session measured in days.
 * The rule is only worth having if something checks it, so this links with
 * `-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free` and fails on
 * the first call rather than counting them.
 *
 * Setup is deliberately outside the armed window: renderer initialisation and
 * graphics init are ALLOWED to allocate -- they happen once, off the frame
 * path -- and only pg_render itself is armed. A test that armed too early
 * would be red for a reason that is not the rule.
 */
#include "pleb_plant_grower.h"

#include "pg_plant.h"
#include "pg_state.h"

#include "kilix_top_down_soft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int armed;
static int violations;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);

static void trip(const char *what)
{
    if (armed) {
        (void)fprintf(stderr, "FAIL noalloc: %s during pg_render\n", what);
        violations += 1;
    }
}

void *__wrap_malloc(size_t size)
{
    trip("malloc");
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    trip("calloc");
    return __real_calloc(count, size);
}

void *__wrap_realloc(void *pointer, size_t size)
{
    trip("realloc");
    return __real_realloc(pointer, size);
}

void __wrap_free(void *pointer)
{
    /* free() during a frame is as much a heap operation as malloc, and it is
     * the one people forget to forbid. */
    trip("free");
    __real_free(pointer);
}

int main(void)
{
    static ki_td_soft_renderer renderer;
    static pg_state state;
    static pg_graphics graphics;
    pg_render_result result;
    size_t species;

    /* --- setup: allocation permitted --- */
    if (!ki_td_soft_renderer_init(&renderer, 960, 540)) {
        (void)fputs("FAIL noalloc: renderer would not initialise\n", stderr);
        return 1;
    }
    if (!pg_graphics_init(&graphics, NULL, NULL, 0u)) {
        (void)fputs("FAIL noalloc: graphics would not initialise\n", stderr);
        return 1;
    }

    /* --- armed: every species, so no branch escapes the check --- */
    for (species = 0u; species < (size_t)PG_SPECIES_COUNT; ++species) {
        pg_init(&state, 4242u);
        state.plant_count = 1u;
        state.anchor.established = true;
        state.anchor.last_wall_s = INT64_C(1813244400);
        pg_plant_init(&state.plants[0], (uint8_t)species,
                      (uint8_t)PG_POT_TERRACOTTA, (uint8_t)PG_SPOT_DEFAULT,
                      state.anchor.last_wall_s);
        state.plants[0].leaf_count = 6u;
        state.plants[0].vine_count =
            (species == (size_t)PG_SPECIES_POTHOS) ? 8u : 0u;
        state.plants[0].growth_stage = 2u;

        armed = 1;
        if (!pg_render(&renderer, &state, &graphics, &result)) {
            armed = 0;
            (void)fputs("FAIL noalloc: pg_render refused\n", stderr);
            return 1;
        }
        armed = 0;
    }

    pg_graphics_shutdown(&graphics);
    ki_td_soft_renderer_destroy(&renderer);

    if (violations != 0) {
        (void)fprintf(stderr, "FAIL noalloc: %d heap operations\n",
                      violations);
        return 1;
    }
    (void)puts("PASS noalloc: the render path performs no allocation");
    return 0;
}
