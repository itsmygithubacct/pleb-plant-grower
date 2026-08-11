#!/usr/bin/env python3
"""Embed guard.

kilix-land links libpleb-plant-grower.a into its own process. The archive must
therefore never pull in a terminal session, an event loop, an exit path, a
signal handler — or a clock. The host owns all of them.

The clock is the one worth naming twice. ARCHITECTURE.md §2.2.3 calls "nothing
in the core reads a clock" the single most important rule in the codebase: time
arrives only in a pg_now the frontend filled, which is what makes a 30-day
catch-up replay bit-exactly and what makes --selftest byte-identical under any
TZ. A core that called time() directly would still build, still pass every unit
test, and quietly destroy that property.

Two things this checks that a name list cannot:

  - **Families, by prefix.** The host surface grows (kittyts_, kittykb_,
    kilix_game_signals_ ...). Matching whole names means every new entry point
    is unguarded until somebody remembers to add it, which is how a guard rots
    into decoration.
  - **Itself.** `--selftest` runs the classifier over symbols that must be
    caught and symbols that must not, so a guard that has stopped guarding
    fails loudly instead of printing PASS. A gate nobody tests is not a gate;
    this file previously matched exact names only and omitted every clock
    symbol, and it still printed PASS against an archive that called them.
"""
from __future__ import annotations

import subprocess
import sys

# Whole symbols. Anything the host must own the only copy of.
FORBIDDEN_EXACT = frozenset({
    # exit paths — the archive may never end the host's process
    "exit", "_exit", "_Exit", "abort", "quick_exit", "atexit", "at_quick_exit",
    # control flow out of the host's stack
    "longjmp", "siglongjmp", "_longjmp",
    # signals belong to the host
    "signal", "sigaction", "raise", "sigprocmask", "pthread_sigmask",
    # the terminal is the host's
    "tcsetattr", "tcgetattr", "tcflush", "tcdrain", "tcsendbreak", "cfmakeraw",
    "ioctl",
    # ---- clocks: the rule this file exists for ----
    "clock", "clock_gettime", "clock_getres", "clock_settime",
    "time", "time64", "gettimeofday", "settimeofday", "times",
    "localtime", "localtime_r", "gmtime", "gmtime_r",
    "mktime", "timegm", "difftime",
    "ctime", "ctime_r", "asctime", "asctime_r", "strftime", "strptime",
    "nanosleep", "clock_nanosleep", "sleep", "usleep",
})

# Symbol families, by prefix. The host's own surfaces.
FORBIDDEN_PREFIXES = (
    "kittyts_",              # terminal session
    "kittyfb_",              # framebuffer / present
    "kittyin_",              # input polling
    "kittykb_",              # keyboard
    "kittyfp_",              # frame presenter
    "kilix_game_host_",      # the host loop
    "kilix_game_signals_",   # the host's signal plumbing
)


def classify(symbol: str) -> str | None:
    """Return the reason `symbol` is forbidden, or None if it is allowed."""
    # nm prints versioned symbols as name@GLIBC_2.17 and name@@GLIBC_2.17.
    bare = symbol.split("@", 1)[0]
    # A few libcs prefix the real entry point.
    for affix in ("__", "_"):
        if bare.startswith(affix) and bare[len(affix):] in FORBIDDEN_EXACT:
            return f"{symbol} (an alias of {bare[len(affix):]})"
    if bare in FORBIDDEN_EXACT:
        return symbol
    for prefix in FORBIDDEN_PREFIXES:
        if bare.startswith(prefix):
            return f"{symbol} (host family {prefix}*)"
    return None


# (symbol, must_be_caught). The negatives matter as much as the positives: a
# guard that rejects memcpy would be abandoned within a week.
SELFTEST_CASES = (
    ("clock_gettime", True),
    ("clock_gettime@GLIBC_2.17", True),
    ("time", True),
    ("time@@GLIBC_2.2.5", True),
    ("localtime_r", True),
    ("gmtime_r", True),
    ("gettimeofday", True),
    ("nanosleep", True),
    ("exit", True),
    ("_exit", True),
    ("abort", True),
    ("tcsetattr", True),
    ("kittyts_start", True),
    ("kittyts_anything_added_later", True),
    ("kittykb_poll", True),
    ("kittyfb_present", True),
    ("kilix_game_host_run", True),
    ("kilix_game_host_start", True),
    ("kilix_game_signals_install", True),
    # allowed: the core is C, and these are not the host's to own
    ("memcpy", False),
    ("memset", False),
    ("memmove", False),
    ("memcmp", False),
    ("strlen", False),
    ("snprintf", False),
    ("malloc", False),
    ("free", False),
    ("fopen", False),
    ("fwrite", False),
    ("open", False),
    ("read", False),
    ("write", False),
    ("kilixstate_save", False),
    ("kilixstate_read_u32", False),
    ("pg_advance", False),
    ("pg_care_tick", False),
    # near-misses that must NOT trip: substring, not prefix
    ("pg_time_credit", False),
    ("pg_calendar_local", False),
    ("pg_sim_env_now", False),
    ("my_kittyts_wrapper", False),
)


def selftest() -> int:
    failures = []
    for symbol, must_catch in SELFTEST_CASES:
        caught = classify(symbol) is not None
        if caught != must_catch:
            failures.append(
                f"  {symbol}: expected {'CAUGHT' if must_catch else 'allowed'}, "
                f"got {'CAUGHT' if caught else 'allowed'}")
    if failures:
        print("embed-guard selftest: the guard does not guard:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    caught = sum(1 for _, m in SELFTEST_CASES if m)
    print(f"embed-guard selftest: PASS ({caught} caught, "
          f"{len(SELFTEST_CASES) - caught} allowed)")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) == 2 and argv[1] == "--selftest":
        return selftest()
    if len(argv) != 3:
        print("usage: check_embed_guard.py <nm> <archive>\n"
              "       check_embed_guard.py --selftest", file=sys.stderr)
        return 2
    nm, archive = argv[1], argv[2]
    proc = subprocess.run([nm, "--undefined-only", archive],
                          capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        print(f"embed-guard: {nm} failed on {archive}", file=sys.stderr)
        return 1
    undefined = {
        line.split()[-1]
        for line in proc.stdout.splitlines()
        if line.strip() and not line.rstrip().endswith(":")
    }
    hits = sorted(filter(None, (classify(s) for s in undefined)))
    if hits:
        print("embed-guard: archive references host-owned symbols:",
              file=sys.stderr)
        for hit in hits:
            print(f"  {hit}", file=sys.stderr)
        return 1
    print(f"embed-guard: PASS ({len(undefined)} undefined symbols, "
          f"none host-owned)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
