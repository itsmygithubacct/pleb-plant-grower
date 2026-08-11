/*
 * Standalone frontend entry point. Not part of the archive kilix-land links:
 * the embed provides its own host, so main() and the terminal session are
 * deliberately excluded from libpleb-plant-grower.a.
 */
#include "pleb_plant_grower.h"

#include <stdio.h>
#include <string.h>

static void usage(void)
{
    (void)puts("usage: pleb-plant-grower [--version] [--help]");
    (void)puts("");
    (void)puts("A realtime houseplant you actually have to look after.");
    (void)puts("Runs standalone, or embedded in kilix-land.");
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
    if (argc > 1) {
        (void)fprintf(stderr, "unknown option: %s\n", argv[1]);
        return 2;
    }
    (void)fputs("pleb-plant-grower: no terminal frontend yet\n", stderr);
    return 0;
}
