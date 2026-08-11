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

CORE_SRC := $(filter-out src/main.c src/pg_term.c,$(wildcard src/*.c))
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

.PHONY: all test test-cli test-unit test-headless test-render test-content \
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

$(BIN): $(BUILD)/obj/main.o $(BUILD)/obj/pg_term.o $(CORE_OBJ) $(VENDOR_OBJ) $(APP_LIBS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BUILD)/obj/main.o $(BUILD)/obj/pg_term.o \
		$(CORE_OBJ) $(VENDOR_OBJ) $(APP_LIBS) $(LDLIBS)

test: test-cli test-unit test-headless test-content test-assets \
      test-backgrounds test-art-review test-save embed-guard originality \
      check-release-tree

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
		[ -e "$$t" ] || continue; found=1; \
		out=$(BUILD)/tests/$$(basename $$t .c); \
		$(CC) $(CPPFLAGS) $(CFLAGS) $$t $(PG_LIB) $(APP_LIBS) $(LDLIBS) -o $$out; \
		$$out; \
	done; \
	if [ $$found -eq 0 ]; then echo "test-unit: no unit tests yet"; \
	else echo "test-unit: PASS"; fi

test-headless: $(BIN)
	@./$(BIN) --rules-test
	@./$(BIN) --calendar-test
	@./$(BIN) --care-test
	@./$(BIN) --time-test
	@dir=$$(mktemp -d); \
	./$(BIN) --selftest 1337 400 > $$dir/a.txt; \
	./$(BIN) --selftest 1337 400 > $$dir/b.txt; \
	cmp $$dir/a.txt $$dir/b.txt || { rm -rf $$dir; exit 1; }; \
	rm -rf $$dir; echo "test-headless: PASS"

test-render: $(BIN)
	@dir=$$(mktemp -d); ./$(BIN) --render-test 7 $$dir && \
		$(PYTHON) tests/test_render.py $$dir; rc=$$?; rm -rf $$dir; exit $$rc

test-content:
	@if [ -x tools/compile_content.py ]; then \
		PYTHONPATH=$(KILIX_GAME_TOOLS_PYTHONPATH) $(PYTHON) tools/compile_content.py --check; \
	else echo "test-content: no content compiler yet"; fi

generated-check:
	@echo "generated-check: pending Milestone 3"

test-assets:
	@if [ -s assets/graphics/manifest.json ]; then \
		PYTHONPATH=$(KILIX_GAME_TOOLS_PYTHONPATH) $(PYTHON) tools/prepare_graphics.py --check; \
	else echo "test-assets: empty manifest, nothing to validate"; fi

test-backgrounds:
	@if [ -x tools/validate_backgrounds.py ]; then \
		$(PYTHON) tools/validate_backgrounds.py; \
	else echo "test-backgrounds: no backgrounds yet"; fi

test-art-review:
	@$(PYTHON) tools/check_art_review.py

test-save: $(BIN)
	@dir=$$(mktemp -d); ./$(BIN) --save-test $$dir; rc=$$?; rm -rf $$dir; \
	test $$rc -eq 0 && echo "test-save: PASS"

embed-guard: $(PG_LIB)
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

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
	        LDFLAGS="-fsanitize=address,undefined" test-cli test-headless

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
