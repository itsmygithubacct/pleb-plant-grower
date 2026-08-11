# The entire kilix-land integration surface on our side.
# Modelled on kilix-ui/mk/kilix-ui.mk.
ifndef PLEB_PLANT_GROWER_MK_INCLUDED
PLEB_PLANT_GROWER_MK_INCLUDED := 1

PG_DEFAULT_GOAL_BEFORE_INCLUDE := $(.DEFAULT_GOAL)

PG_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
PG_BUILD_DIR ?= $(PG_ROOT)/build
PG_LIB := $(PG_BUILD_DIR)/libpleb-plant-grower.a
PG_CPPFLAGS := -I$(PG_ROOT)/include
PG_INPUTS := $(wildcard $(PG_ROOT)/src/*.c) $(wildcard $(PG_ROOT)/src/*.h) \
             $(PG_ROOT)/include/pleb_plant_grower.h

$(PG_LIB): $(PG_INPUTS)
	$(MAKE) -C $(PG_ROOT) BUILD="$(PG_BUILD_DIR)" "$@"

ifeq ($(PG_DEFAULT_GOAL_BEFORE_INCLUDE),)
.DEFAULT_GOAL :=
endif

endif
