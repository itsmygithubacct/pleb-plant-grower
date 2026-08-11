/*
 * Standalone frontend entry point. Not part of the archive kilix-land links:
 * the embed provides its own host, so main() and the terminal session are
 * deliberately excluded from libpleb-plant-grower.a.
 *
 * The dispatch is a table rather than a strcmp chain (IMPLEMENTATION_PLAN.md
 * §5.6). That is not tidiness: a chain accepts `--version --help` by silently
 * ignoring the second argument, and it has nowhere to state an arity, so
 * `--selftest abc def` ran seed 0 for 0 days and exited 0 — a typo'd gate that
 * looks exactly like a pass. Every row now declares how many arguments it
 * takes and every numeric argument is parsed strictly.
 */
#include "pleb_plant_grower.h"

#include "pg_calendar.h"
#include "pg_care.h"
#include "pg_plant.h"
#include "pg_render.h"
#include "pg_save.h"
#include "pg_sim.h"
#include "pg_time.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    (void)puts("usage: pleb-plant-grower [--version] [--help]");
    (void)puts("                         [--calendar-test] [--time-test]");
    (void)puts("                         [--care-test] [--rules-test]");
    (void)puts("                         [--selftest [<seed> [<sim-days>]]]");
    (void)puts("                         [--save-test <dir>]");
    (void)puts("                         [--render-test <seed> <dir>]");
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
    (void)puts("                   arguments, so two runs compare equal.");
    (void)puts("                   Defaults: seed 1337, 400 days");
    (void)puts("  --save-test      persistence: the record round trips, then every");
    (void)puts("                   single-bit flip, truncation and semantic case is");
    (void)puts("                   either refused or decodes to a record that");
    (void)puts("                   re-encodes to itself — and neither outcome ever");
    (void)puts("                   changes the plant it was handed. <dir> must be an");
    (void)puts("                   absolute path; it is written into and then emptied");
}

/* strtoull returns 0 on garbage, so a bare call cannot tell `0` from `abc`.
 * Reject an empty string, trailing text and a range error -- and a leading
 * sign, because strtoull negates rather than refusing it: `-1` would otherwise
 * be accepted as 18446744073709551615, which is a typo silently becoming a
 * legal seed. */
static bool parse_u64(const char *text, unsigned long long limit,
                      unsigned long long *out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || *text == '\0' || *text == '-' || *text == '+') {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE || value > limit) {
        return false;
    }
    *out = value;
    return true;
}

static int cmd_version(int extra, char **args)
{
    (void)extra; (void)args;
    (void)printf("pleb-plant-grower %s\n", pg_version());
    return 0;
}

static int cmd_help(int extra, char **args)
{
    (void)extra; (void)args;
    usage();
    return 0;
}

static int cmd_calendar(int extra, char **args)
{
    (void)extra; (void)args;
    return pg_calendar_run_test();
}

static int cmd_time(int extra, char **args)
{
    (void)extra; (void)args;
    return pg_time_run_test();
}

static int cmd_care(int extra, char **args)
{
    (void)extra; (void)args;
    return pg_care_run_test();
}

static int cmd_rules(int extra, char **args)
{
    (void)extra; (void)args;
    return pg_plant_run_rules_test();
}

/* Both arguments are optional and defaulted, so --selftest on its own is still
 * a meaningful gate. Neither may be garbage. */
static int cmd_selftest(int extra, char **args)
{
    unsigned long long seed = 1337ull;
    unsigned long long days = 400ull;

    if (extra >= 1 && !parse_u64(args[0], UINT64_MAX, &seed)) {
        (void)fprintf(stderr, "--selftest: seed '%s' is not a number\n",
                      args[0]);
        return 2;
    }
    if (extra >= 2 && !parse_u64(args[1], UINT32_MAX, &days)) {
        (void)fprintf(stderr,
                      "--selftest: sim-days '%s' is not a number in 0..%u\n",
                      args[1], (unsigned)UINT32_MAX);
        return 2;
    }
    return pg_sim_run_selftest((uint64_t)seed, (uint32_t)days);
}

static int cmd_save_test(int extra, char **args)
{
    (void)extra;
    return pg_save_run_test(args[0]);
}

/* <seed> <dir>: both required, because a render fixture set that silently
 * defaulted its directory would write into the working tree. */
static int cmd_render_test(int extra, char **args)
{
    unsigned long long seed = 0ull;

    (void)extra;
    if (!parse_u64(args[0], UINT64_MAX, &seed)) {
        (void)fprintf(stderr, "--render-test: seed '%s' is not a number\n",
                      args[0]);
        return 2;
    }
    return pg_render_run_test((uint64_t)seed, args[1]);
}

typedef struct pg_command {
    const char *name;
    int min_extra;
    int max_extra;
    const char *argspec;                  /* for the arity diagnostic */
    int (*run)(int extra, char **args);
} pg_command;

static const pg_command COMMANDS[] = {
    { "--version",       0, 0, "",                        cmd_version },
    { "--help",          0, 0, "",                        cmd_help },
    { "--calendar-test", 0, 0, "",                        cmd_calendar },
    { "--time-test",     0, 0, "",                        cmd_time },
    { "--care-test",     0, 0, "",                        cmd_care },
    { "--rules-test",    0, 0, "",                        cmd_rules },
    { "--selftest",      0, 2, " [<seed> [<sim-days>]]",  cmd_selftest },
    { "--save-test",     1, 1, " <dir>",                  cmd_save_test },
    { "--render-test",   2, 2, " <seed> <dir>",           cmd_render_test },
};

int main(int argc, char **argv)
{
    size_t index;

    if (argc <= 1) {
        (void)fputs("pleb-plant-grower: no terminal frontend yet\n", stderr);
        return 0;
    }
    for (index = 0; index < sizeof COMMANDS / sizeof COMMANDS[0]; ++index) {
        const pg_command *command = &COMMANDS[index];
        int extra;

        if (strcmp(argv[1], command->name) != 0) {
            continue;
        }
        extra = argc - 2;
        if (extra < command->min_extra || extra > command->max_extra) {
            (void)fprintf(stderr, "usage: pleb-plant-grower %s%s\n",
                          command->name, command->argspec);
            return 2;
        }
        return command->run(extra, &argv[2]);
    }
    (void)fprintf(stderr, "unknown option: %s\n", argv[1]);
    return 2;
}
