# The entire kilix-land integration surface on our side.
# Modelled on kilix-ui/mk/kilix-ui.mk.
ifndef PLEB_PLANT_GROWER_MK_INCLUDED
PLEB_PLANT_GROWER_MK_INCLUDED := 1

PG_DEFAULT_GOAL_BEFORE_INCLUDE := $(.DEFAULT_GOAL)

PG_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
PG_BUILD_DIR ?= $(PG_ROOT)/build
PG_LIB := $(PG_BUILD_DIR)/libpleb-plant-grower.a
# BOTH include/ and src/. The public header names pg_state, pg_graphics,
# pg_settings and pg_store but does not define them: their layouts live in
# src/pg_state.h, and a host that embeds pg_state BY VALUE -- which is the
# contract, the way kilix-land holds land_game, with no allocator anywhere --
# needs that definition. Exposing only include/ makes the documented embed
# impossible to compile, which is what it did until 2026-08-11.
#
# The price is stated in docs/EMBEDDING.md and is not negotiable: the host and
# this archive share an aggregate layout, so they must be rebuilt together from
# one commit. A host holding a stale pg_state and a new libpleb-plant-grower.a
# corrupts its own stack. An embedder that only wants the SIZE, and will supply
# its own storage, needs just include/ and pg_state_size().
PG_CPPFLAGS := -I$(PG_ROOT)/include -I$(PG_ROOT)/src
PG_INPUTS := $(wildcard $(PG_ROOT)/src/*.c) $(wildcard $(PG_ROOT)/src/*.h) \
             $(PG_ROOT)/include/pleb_plant_grower.h

$(PG_LIB): $(PG_INPUTS)
	$(MAKE) -C $(PG_ROOT) BUILD="$(PG_BUILD_DIR)" "$@"

ifeq ($(PG_DEFAULT_GOAL_BEFORE_INCLUDE),)
.DEFAULT_GOAL :=
endif

endif
