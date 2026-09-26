# OpenMW-N64 — builds openmw_n64.z64 with libdragon.
#
#   export N64_INST=/opt/libdragon          # libdragon *preview* branch
#   make OSG_SRC=/path/to/OpenSceneGraph    # 3.6.5 source tree (headers + 2 .cpp)
#
# Everything under components/ is compiled straight from the OpenMW tree; the
# only N64-specific code lives in this directory.

OPENMW_ROOT := ../..
OSG_SRC     ?= /opt/src/osg
BUILD_DIR   := build
SOURCE_DIR  := .

.DEFAULT_GOAL := all

include $(N64_INST)/include/n64.mk

# OpenMW needs C++20; n64.mk appends -std=gnu++17 per-target, so swap it out.
# OpenMW's sources are not warning-clean under libdragon's -Werror set.
N64_CXXFLAGS := $(filter-out -std=gnu++17 -Werror,$(N64_CXXFLAGS)) -std=gnu++20

# --- OpenMW component sources (unmodified apart from the endian fixes) -------
C := $(OPENMW_ROOT)/components
OPENMW_SRCS := \
	$(C)/bsa/bsafile.cpp \
	$(C)/esm/format.cpp \
	$(C)/esm/refid.cpp \
	$(C)/esm/stringrefid.cpp \
	$(C)/esm/generatedrefid.cpp \
	$(C)/esm/indexrefid.cpp \
	$(C)/esm/formid.cpp \
	$(C)/esm/esm3exteriorcellrefid.cpp \
	$(C)/esm3/esmreader.cpp \
	$(C)/esm3/readerscache.cpp \
	$(C)/esm3/loadtes3.cpp \
	$(C)/esm3/loadcell.cpp \
	$(C)/esm3/cellref.cpp \
	$(C)/esm3/cellid.cpp \
	$(C)/nif/niffile.cpp \
	$(C)/nif/nifstream.cpp \
	$(C)/nif/base.cpp \
	$(C)/nif/controller.cpp \
	$(C)/nif/data.cpp \
	$(C)/nif/effect.cpp \
	$(C)/nif/extra.cpp \
	$(C)/nif/node.cpp \
	$(C)/nif/particle.cpp \
	$(C)/nif/physics.cpp \
	$(C)/nif/property.cpp \
	$(C)/nif/texture.cpp \
	$(C)/toutf8/toutf8.cpp \
	$(C)/files/constrainedfilestreambuf.cpp \
	$(C)/files/conversion.cpp \
	$(C)/files/openfile.cpp \
	$(C)/files/hash.cpp \
	$(C)/platform/filestdio.cpp \
	$(C)/debug/debuglog.cpp \
	$(OPENMW_ROOT)/extern/smhasher/MurmurHash3.cpp

# The two OSG translation units the math headers need out-of-line code from.
OSG_SRCS := $(OSG_SRC)/src/osg/Quat.cpp $(OSG_SRC)/src/osg/Matrixf.cpp

APP_SRCS := $(wildcard *.cpp)

INCLUDES := -I. -Icompat -I$(OPENMW_ROOT) -I$(OPENMW_ROOT)/components -I$(OSG_SRC)/include -I$(OPENMW_ROOT)/extern -I$(OPENMW_ROOT)/extern/smhasher/include -I$(OPENMW_ROOT)/extern/smhasher/include/smhasher
CXXFLAGS += $(INCLUDES) -DOPENMW_N64 -Os -Wno-deprecated-declarations $(EXTRA_CXXFLAGS)
CFLAGS   += $(INCLUDES) -Os
ifeq ($(DISPLAY_LISTS),1)
CXXFLAGS += -DOMW64_DISPLAY_LISTS
endif
ifeq ($(PAGED),1)
CXXFLAGS += -DOMW64_PAGED_DEFAULT
endif
ifeq ($(AUTOPLAY),1)
CXXFLAGS += -DOMW64_AUTOPLAY
ifneq ($(AUTOPLAY_CELL),)
CXXFLAGS += -DOMW64_AUTOPLAY_CELL='"$(AUTOPLAY_CELL)"'
endif
ifneq ($(AUTOPLAY_YAW),)
CXXFLAGS += -DOMW64_AUTOPLAY_YAW=$(AUTOPLAY_YAW) -DOMW64_AUTOPLAY_PITCH=$(or $(AUTOPLAY_PITCH),0)
endif
endif

# Map every source to a unique object under build/.
obj = $(BUILD_DIR)/$(subst ../,,$(subst $(OSG_SRC)/,osg/,$(1:.cpp=.o)))
OBJS := $(foreach s,$(APP_SRCS) $(OPENMW_SRCS) $(OSG_SRCS),$(call obj,$(s)))

define compile_rule
$(call obj,$(1)): $(1)
	@mkdir -p $$(dir $$@)
	@echo "    [CXX] $$<"
	$$(CXX) -c $$(CXXFLAGS) -o $$@ $$<
endef
$(foreach s,$(APP_SRCS) $(OPENMW_SRCS) $(OSG_SRCS),$(eval $(call compile_rule,$(s))))

all: openmw_n64.z64

# Optional test data packed into the ROM filesystem (rom:/). Real Morrowind
# data is read from the flashcart SD card instead (sd:/).
ifneq ($(wildcard filesystem/*),)
openmw_n64.z64: $(BUILD_DIR)/openmw_n64.dfs
$(BUILD_DIR)/openmw_n64.dfs: $(shell find filesystem -type f 2>/dev/null)
endif

openmw_n64.z64: N64_ROM_TITLE = "OpenMW N64"
openmw_n64.z64: N64_ROM_SAVETYPE = none
$(BUILD_DIR)/openmw_n64.elf: $(OBJS)

objs: CXX=$(N64_CXX)
objs: CXXFLAGS+=$(N64_CXXFLAGS)
objs: $(OBJS)

clean:
	rm -rf $(BUILD_DIR) openmw_n64.z64

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)

.PHONY: all clean objs
