# Shared build fragments define internal archive targets; bare `make` must
# still build the runnable game.
.DEFAULT_GOAL := all

CC      ?= cc
NM      ?= nm
PYTHON  ?= python3
BUILD   ?= build
BIN     ?= pleb-plant-grower
PREFIX  ?= /usr/local
DESTDIR ?=
VERSION := 0.1.0

KILIX_GAME_SDK_DIR ?= third_party/kilix-game-sdk
KILIX_GAME_KIT_DIR ?= $(KILIX_GAME_SDK_DIR)/kilix-game-kit
# game-kit.mk derives its compatibility paths from the most recently included
# makefile, so this root must be frozen before any further include
# (<games>/shellda/Makefile:3-9).
ifndef KILIX_GAME_KIT_ROOT
KILIX_GAME_KIT_ROOT := $(abspath $(KILIX_GAME_KIT_DIR))
endif

SDK_BUILD := $(abspath $(BUILD)/sdk)
KILIX_GAME_KIT_BUILD_DIR ?= $(SDK_BUILD)/game-kit
KILIX_TOP_DOWN_BUILD_DIR ?= $(SDK_BUILD)/top-down
KILIX_UI_BUILD_DIR       ?= $(SDK_BUILD)/ui
KILIX_ASSETS_BUILD_DIR   ?= $(SDK_BUILD)/assets
KILIX_STORY_BUILD_DIR    ?= $(SDK_BUILD)/story

include $(KILIX_GAME_KIT_DIR)/mk/game-kit.mk
KILIX_TOP_DOWN_DIR ?= $(KILIX_GAME_SDK_DIR)/kilix-top-down-engine
include $(KILIX_TOP_DOWN_DIR)/mk/kilix-top-down.mk
KILIX_UI_DIR ?= $(KILIX_GAME_SDK_DIR)/kilix-ui
include $(KILIX_UI_DIR)/mk/kilix-ui.mk
KILIX_ASSETS_DIR ?= $(KILIX_GAME_SDK_DIR)/kilix-assets
include $(KILIX_ASSETS_DIR)/mk/kilix-assets.mk
KILIX_STORY_DIR ?= $(KILIX_GAME_SDK_DIR)/kilix-story
include $(KILIX_STORY_DIR)/mk/kilix-story.mk

CHIP_SEQUENCER_DIR ?= third_party/chip-sequencer
KILIX_GAME_TOOLS_DIR ?= $(KILIX_GAME_SDK_DIR)/kilix-game-tools
KILIX_GAME_TOOLS_PYTHONPATH := $(abspath $(KILIX_GAME_TOOLS_DIR))/src

override CPPFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	-Iinclude -Isrc -I$(BUILD) \
	$(KILIX_GAME_KIT_CPPFLAGS) $(KILIX_TD_CPPFLAGS) $(KILIX_UI_CPPFLAGS) \
	$(KILIX_ASSETS_CPPFLAGS) $(KILIX_STORY_CPPFLAGS) \
	-I$(CHIP_SEQUENCER_DIR)/include
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11
# Growth math must be byte-reproducible: no fused multiply-add rewrites.
override CFLAGS += -ffp-contract=off
GAME_CFLAGS = -Wshadow -Wconversion
STRICT_CFLAGS := -O2 -g -std=c11 -Wall -Wextra -Wpedantic -Werror \
	-Wconversion -Wsign-conversion -Wshadow -Wformat=2
LDLIBS  ?= $(KILIX_ASSETS_LDLIBS) $(KILIX_GAME_KIT_LDLIBS)

GENERATED_HEADER := $(BUILD)/pg_content_generated.h
CONTENT_JSON := $(wildcard content/*.json)

# pg_render_test.c is a harness, not core: it is linked into the standalone
# binary only, so the archive kilix-land embeds does not pull in the kit's
# test library.
CORE_SRC := $(filter-out src/main.c src/pg_term.c src/pg_render_test.c,$(wildcard src/*.c))
KILIX_GAME_KIT_TEST_LIB := $(KILIX_GAME_KIT_BUILD_DIR)/libkilix-game-test.a
CORE_OBJ := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(CORE_SRC))
VENDOR_OBJ := $(BUILD)/obj/chip_sequencer.o
PG_LIB   := $(BUILD)/libpleb-plant-grower.a
APP_LIBS := $(KILIX_UI_LIB) $(KILIX_TD_LIBS) $(KILIX_ASSETS_LIB) \
            $(KILIX_STORY_LIB) $(KILIX_GAME_KIT_LIB)

# --- originality gate (ARCHITECTURE.md §9, verbatim) -----------------------
ORIG_PATHS    := src include docs tools tests content packaging assets \
                 README.md CHANGELOG.md LICENSE Makefile
ORIG_ATTRIB   := (claude|anthropic)
ORIG_HOMEPATH := /home/[A-Za-z0-9_]
ORIG_HOSTS    := (^|[^A-Za-z0-9_-])(p50|opib|plebtop|plebdesk)([^A-Za-z0-9_-]|$$)
ORIG_HOSTCTX  := (neon:|@neon([^A-Za-z0-9_-]|$$)|neon\.(local|lan))
ORIG_EMAIL    := [A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z][A-Za-z]+
ORIG_EMAIL_OK := itsmygithubacct@users\.noreply\.github\.com
# Required, and the reason the gate can pass at all: ORIG_PATHS includes
# tests, and tests/fixtures/originality/ exists precisely to hold strings every
# one of these patterns must match. The positive cases are evaluated in exactly
# one place, tests/test_release_tree.py, and never by this target.
ORIG_SKIP     := --exclude-dir=fixtures
# The gate's own pattern definitions live in this Makefile and necessarily
# contain every token they match. Exempt exactly those lines — by file and by
# the ORIG_ prefix — so the definitions do not fail the gate while any other
# line of the Makefile is still scanned.
ORIG_SELF     := ^Makefile:[0-9]+:ORIG_[A-Z_]+ +:=

.PHONY: all test test-cli test-unit test-noalloc test-headless test-render \
        test-content \
        generated-check test-assets test-backgrounds test-art-review test-save \
        embed-guard check-release-tree originality art sfx check-sfx sanitize \
        test-clang test-shared install uninstall release-gate dist clean

all: $(BIN)

$(BUILD) $(BUILD)/obj $(BUILD)/tests:
	@mkdir -p $@

$(VENDOR_OBJ): $(CHIP_SEQUENCER_DIR)/src/chip_sequencer.c | $(BUILD)/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/%.o: src/%.c | $(BUILD)/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) $(GAME_CFLAGS) -c $< -o $@

# content/*.json is the authored source; the header is a build product and is
# regenerated whenever the JSON moves, so it can never be stale in the tree.
$(GENERATED_HEADER): $(CONTENT_JSON) tools/compile_content.py \
                     tools/check_care_schedule.py | $(BUILD)
	$(PYTHON) tools/compile_content.py --out $@

$(CORE_OBJ) $(BUILD)/obj/main.o: $(GENERATED_HEADER)

# Per-object SDK header dependencies, declared by hand (house style).
$(BUILD)/obj/pg_render.o: $(SOFT_RASTER_DIR)/include/soft_raster.h
$(BUILD)/obj/pg_audio.o:  $(PCM_MIXER_DIR)/include/pcmmix_bank.h \
                          $(CHIP_SEQUENCER_DIR)/include/chip_sequencer.h
$(BUILD)/obj/pg_store.o:  $(KILIX_STATE_DIR)/include/kilix_state_codec.h
$(BUILD)/obj/pg_term.o:   $(KITTY_INPUT_DIR)/include/kitty_input.h \
                          $(KITTY_FRAMEBUFFER_DIR)/include/kitty_framebuffer.h \
                          $(KITTY_TERMINAL_SESSION_DIR)/include/kitty_terminal_session.h

$(PG_LIB): $(CORE_OBJ) $(VENDOR_OBJ) | $(BUILD)
	ar rcs $@ $(CORE_OBJ) $(VENDOR_OBJ)

$(KILIX_GAME_KIT_TEST_LIB): $(KILIX_GAME_KIT_LIB)
	@$(MAKE) --no-print-directory -C $(KILIX_GAME_KIT_DIR) \
		BUILD_DIR=$(KILIX_GAME_KIT_BUILD_DIR) $@

$(BIN): $(BUILD)/obj/main.o $(BUILD)/obj/pg_term.o $(BUILD)/obj/pg_render_test.o \
        $(CORE_OBJ) $(VENDOR_OBJ) $(APP_LIBS) $(KILIX_GAME_KIT_TEST_LIB)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BUILD)/obj/main.o $(BUILD)/obj/pg_term.o \
		$(BUILD)/obj/pg_render_test.o \
		$(CORE_OBJ) $(VENDOR_OBJ) $(APP_LIBS) $(KILIX_GAME_KIT_TEST_LIB) $(LDLIBS)

test: test-cli test-unit test-noalloc test-headless test-render test-content \
      test-assets test-backgrounds test-art-review test-save check-sfx \
      embed-guard originality check-release-tree

test-cli: $(BIN)
	@./$(BIN) --version >/dev/null
	@./$(BIN) --help    >/dev/null
	@if ./$(BIN) --no-such-flag >/dev/null 2>&1; then \
		echo "bad argument must exit 2" >&2; exit 1; fi; \
	./$(BIN) --no-such-flag >/dev/null 2>&1; \
	test $$? -eq 2 || { echo "bad argument must exit 2" >&2; exit 1; }
	@echo "test-cli: PASS"

test-unit: $(PG_LIB) | $(BUILD)/tests
	@set -e; found=0; \
	for t in tests/test_*.c; do \
		[ -e "$$t" ] || continue; \
		case "$$t" in tests/test_noalloc.c) continue;; esac; \
		found=1; \
		out=$(BUILD)/tests/$$(basename $$t .c); \
		$(CC) $(CPPFLAGS) $(CFLAGS) $$t $(PG_LIB) $(APP_LIBS) $(LDLIBS) -o $$out; \
		$$out; \
	done; \
	if [ $$found -eq 0 ]; then echo "test-unit: no unit tests yet"; \
	else echo "test-unit: PASS"; fi

test-headless: $(BIN)
	@./$(BIN) --rules-test
	@./$(BIN) --advice-test
	@./$(BIN) --sound-test
	@./$(BIN) --calendar-test
	@./$(BIN) --care-test
	@./$(BIN) --time-test
	@dir=$$(mktemp -d); \
	./$(BIN) --selftest 1337 400 > $$dir/a.txt; \
	./$(BIN) --selftest 1337 400 > $$dir/b.txt; \
	cmp $$dir/a.txt $$dir/b.txt || { rm -rf $$dir; exit 1; }; \
	rm -rf $$dir; echo "test-headless: PASS"

# The no-allocation gate needs the wrap flags, so it cannot ride the generic
# unit-test loop above.
test-noalloc: $(PG_LIB) | $(BUILD)/tests
	@$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_noalloc.c $(PG_LIB) $(APP_LIBS) \
		$(LDLIBS) -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free \
		-o $(BUILD)/tests/test-noalloc
	@$(BUILD)/tests/test-noalloc

test-render: $(BIN)
	@dir=$$(mktemp -d); ./$(BIN) --render-test 7 $$dir && \
		$(PYTHON) tests/test_render.py $$dir; rc=$$?; rm -rf $$dir; exit $$rc

test-content: $(GENERATED_HEADER)
	@PYTHONPATH=$(KILIX_GAME_TOOLS_PYTHONPATH) $(PYTHON) tools/check_care_schedule.py
	@PYTHONPATH=$(KILIX_GAME_TOOLS_PYTHONPATH) $(PYTHON) tools/compile_content.py \
		--out $(GENERATED_HEADER) --check
	@PYTHONPATH=$(KILIX_GAME_TOOLS_PYTHONPATH) $(PYTHON) tests/test_content.py
	@$(MAKE) --no-print-directory generated-check
	@echo "test-content: PASS"

# Recompile into a temporary directory and cmp, so a hand-edited generated
# header fails the build rather than silently disagreeing with content/.
generated-check: $(GENERATED_HEADER)
	@dir=$$(mktemp -d); \
	$(PYTHON) tools/compile_content.py --out $$dir/pg_content_generated.h >/dev/null; \
	cmp $$dir/pg_content_generated.h $(GENERATED_HEADER); rc=$$?; \
	rm -rf $$dir; \
	test $$rc -eq 0 || { echo "generated-check: header does not match content/" >&2; exit 1; }
	@echo "generated-check: PASS"

# Three outcomes, deliberately. An empty manifest is the pre-art state and
# passes; a populated manifest with no pipeline to check it is a FAILURE, not a
# skip -- that combination means art shipped without validation, which is the
# one case a silent "nothing to validate" must never cover.
test-assets:
	@if ! $(PYTHON) -c 'import json,sys; sys.exit(0 if json.load(open("assets/graphics/manifest.json")).get("entries") else 1)' 2>/dev/null; then \
		echo "test-assets: manifest has no entries yet, nothing to validate"; \
	elif [ -f tools/prepare_graphics.py ]; then \
		PYTHONPATH=$(KILIX_GAME_TOOLS_PYTHONPATH) $(PYTHON) tools/prepare_graphics.py --check; \
	else \
		echo "test-assets: manifest lists entries but tools/prepare_graphics.py is missing -- art cannot ship unvalidated" >&2; \
		exit 1; \
	fi

test-backgrounds:
	@if [ -x tools/validate_backgrounds.py ]; then \
		$(PYTHON) tools/validate_backgrounds.py; \
	else echo "test-backgrounds: no backgrounds yet"; fi

test-art-review:
	@$(PYTHON) tools/check_art_review.py

test-save: $(BIN)
	@dir=$$(mktemp -d); ./$(BIN) --save-test $$dir; rc=$$?; rm -rf $$dir; \
	test $$rc -eq 0 && echo "test-save: PASS"

# The guard checks itself before it checks the archive: a classifier that has
# stopped catching clock and host symbols would otherwise print PASS forever.
embed-guard: $(PG_LIB)
	@$(PYTHON) tests/check_embed_guard.py --selftest
	@$(PYTHON) tests/check_embed_guard.py $(NM) $(PG_LIB)

check-release-tree:
	@$(PYTHON) tests/test_release_tree.py

originality:
	@! grep -RIn -i -E '$(ORIG_ATTRIB)'   $(ORIG_SKIP) $(ORIG_PATHS) | grep -v -E '$(ORIG_SELF)'
	@! grep -RIn    -E '$(ORIG_HOMEPATH)' $(ORIG_SKIP) $(ORIG_PATHS) | grep -v -E '$(ORIG_SELF)'
	@! grep -RIn    -E '$(ORIG_HOSTS)'    $(ORIG_SKIP) $(ORIG_PATHS) | grep -v -E '$(ORIG_SELF)'
	@! grep -RIn    -E '$(ORIG_HOSTCTX)'  $(ORIG_SKIP) $(ORIG_PATHS) | grep -v -E '$(ORIG_SELF)'
	@! grep -RIn    -E '$(ORIG_EMAIL)'    $(ORIG_SKIP) $(ORIG_PATHS) | grep -v -E '$(ORIG_EMAIL_OK)' | grep -v -E '$(ORIG_SELF)'
	@echo "originality: PASS"

art:
	$(PYTHON) tools/gen_art.py

sfx:
	$(PYTHON) tools/gen_sfx.py --out assets/sfx --manifest docs/audio-provenance.json

check-sfx:
	$(PYTHON) tools/gen_sfx.py --check --out assets/sfx --manifest docs/audio-provenance.json

# -fno-sanitize-recover is load-bearing: without it UBSan PRINTS the diagnostic
# and carries on, the target exits 0, and the gate reports success over a live
# signed-overflow. That is the exact defect class ARCHITECTURE.md §8.3 makes
# this a gate for -- pg_time's saturating arithmetic exists because a legal
# INT64_MIN -> INT64_MAX wall transition overflows plain subtraction.
# ASAN_OPTIONS/UBSAN_OPTIONS are belt and braces for the same reason.
SANITIZE_CFLAGS := -O1 -g -fsanitize=address,undefined \
                   -fno-sanitize-recover=all -fno-omit-frame-pointer
SANITIZE_LDFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all

sanitize:
	$(MAKE) clean
	ASAN_OPTIONS=abort_on_error=1:halt_on_error=1:detect_leaks=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) CFLAGS="$(SANITIZE_CFLAGS)" LDFLAGS="$(SANITIZE_LDFLAGS)" \
	        test-cli test-headless test-unit test-save

test-clang:
	$(MAKE) clean
	$(MAKE) CC=clang CFLAGS="$(STRICT_CFLAGS)" test

test-shared:
	$(MAKE) -C $(KILIX_GAME_KIT_ROOT) test
	$(MAKE) -C $(abspath $(KILIX_UI_DIR)) test
	$(MAKE) -C $(abspath $(KILIX_ASSETS_DIR)) test
	$(MAKE) -C $(abspath $(KILIX_STORY_DIR)) test
	$(MAKE) -C $(abspath $(KILIX_TOP_DOWN_DIR)) test

install: all
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm644 docs/pleb-plant-grower.6 \
		$(DESTDIR)$(PREFIX)/share/man/man6/pleb-plant-grower.6
	@find assets -type f -print0 | while IFS= read -r -d '' f; do \
		install -Dm644 "$$f" "$(DESTDIR)$(PREFIX)/share/$(BIN)/$$f"; done

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -f $(DESTDIR)$(PREFIX)/share/man/man6/pleb-plant-grower.6
	rm -rf $(DESTDIR)$(PREFIX)/share/$(BIN)

release-gate: test test-shared test-clang sanitize embed-guard originality \
              check-release-tree test-art-review
	@echo "release-gate: PASS"

dist: all
	@tar --sort=name --owner=0 --group=0 --numeric-owner \
		--mtime='@0' -czf $(BIN)-$(VERSION).tar.gz \
		--exclude='./build' --exclude='./.git' .

clean:
	rm -rf $(BUILD) $(BIN)
