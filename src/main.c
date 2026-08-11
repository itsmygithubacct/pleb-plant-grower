/*
 * Standalone frontend entry point. Not part of the archive kilix-land links:
 * the embed provides its own host, so main() and the terminal session are
 * deliberately excluded from libpleb-plant-grower.a.
 */
#include "pleb_plant_grower.h"

#include "pg_calendar.h"
#include "pg_care.h"
#include "pg_plant.h"
#include "pg_save.h"
#include "pg_sim.h"
#include "pg_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    (void)puts("usage: pleb-plant-grower [--version] [--help]");
    (void)puts("                         [--calendar-test] [--time-test]");
    (void)puts("                         [--care-test] [--rules-test]");
    (void)puts("                         [--selftest <seed> <sim-days>]");
    (void)puts("                         [--save-test <dir>]");
    (void)puts("");
    (void)puts("A realtime houseplant you actually have to look after.");
    (void)puts("Runs standalone, or embedded in kilix-land.");
    (void)puts("");
    (void)puts("  --calendar-test  date arithmetic, the season window and the");
    (void)puts("                   local-hour rules; output is independent of TZ");
    (void)puts("  --time-test      the gap matrix: every clock the policy survives");
    (void)puts("  --care-test      the care model: derived watering intervals, the");
    (void)puts("                   symptom ladders, the cachepot's soggy ticks");
    (void)puts("  --rules-test     verb legality, the four hard refusals and the");
    (void)puts("                   myth blocklist");
    (void)puts("  --selftest       the catch-up engine: the same gap simulated in");
    (void)puts("                   ticks, in one step and in ragged chunks must");
    (void)puts("                   agree, and no gap may cost more than 30 days");
    (void)puts("                   of work. Output is a pure function of its two");
    (void)puts("                   arguments, so two runs compare equal");
    (void)puts("  --save-test      persistence: the record round trips, then");
    (void)puts("                   every single-bit flip, every truncation and");
    (void)puts("                   every semantic case is refused without ever");
    (void)puts("                   changing the plant it was handed. <dir> must");
    (void)puts("                   be an absolute path and is written into");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        (void)printf("pleb-plant-grower %s\n", pg_version());
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--calendar-test") == 0) {
        return pg_calendar_run_test();
    }
    if (argc > 1 && strcmp(argv[1], "--time-test") == 0) {
        return pg_time_run_test();
    }
    if (argc > 1 && strcmp(argv[1], "--care-test") == 0) {
        return pg_care_run_test();
    }
    if (argc > 1 && strcmp(argv[1], "--rules-test") == 0) {
        return pg_plant_run_rules_test();
    }
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        /* Both arguments are optional and defaulted, so --selftest on its own
         * is still a meaningful gate. */
        unsigned long long seed = 1337ull;
        unsigned long days = 400ul;
        if (argc > 2) {
            seed = strtoull(argv[2], NULL, 10);
        }
        if (argc > 3) {
            days = strtoul(argv[3], NULL, 10);
        }
        return pg_sim_run_selftest((uint64_t)seed, (uint32_t)days);
    }
    if (argc > 1 && strcmp(argv[1], "--save-test") == 0) {
        if (argc < 3) {
            (void)fputs("--save-test needs an absolute directory\n", stderr);
            return 2;
        }
        return pg_save_run_test(argv[2]);
    }
    if (argc > 1) {
        (void)fprintf(stderr, "unknown option: %s\n", argv[1]);
        return 2;
    }
    (void)fputs("pleb-plant-grower: no terminal frontend yet\n", stderr);
    return 0;
}
