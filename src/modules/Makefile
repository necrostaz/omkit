# Build a module (Linux or WSL; docs/build.md): make -C OMKit/src/modules MOD=hello VERSION=1.0  -> OMKit/build/modules/HELLO.OMM
# A module sees only src/sdk/include: the public API, no driver and no firmware addresses.
# Strings: <mod>/res/strings.txt -> res_strings.h with STR_NAME ids (in the build directory), resources: <mod>/res/, <mod>/res/disk/
# MODDIR: the module's source folder (default <mod> here), for a module kept outside this tree:
#   make -C OMKit/src/modules MOD=mymod MODDIR=/path/to/mymod OUT=/path/to/out
# BUILD=release (the default) or debug: the module's kind, as core's (OMKit/Makefile). A debug build is compiled with
# OMK_DEBUG - the module's lines of the debug level (OMK_LOG_D) and whatever else it keeps for development - into
# OMKit/build/debug/modules
MOD ?= hello
MODDIR ?= $(MOD)
VERSION ?= 1.0
BUILD ?= release
ifeq ($(filter release debug,$(BUILD)),)
$(error BUILD=$(BUILD): release or debug)
endif
ifeq ($(BUILD),debug)
OUT ?= ../../build/debug/modules
KIND_CFLAGS := -DOMK_DEBUG
else
OUT ?= ../../build/modules
KIND_CFLAGS :=
endif
TC := $(HOME)/opt/mn10300/bin/mn10300-elf-
PYTHON ?= python3
TOOLS := ../../tools
RESOURCES := ../../resources/modules
UNAME := $(shell echo $(MOD) | tr a-z A-Z)
GEN := $(OUT)/$(MOD).gen
STRINGS := $(wildcard $(MODDIR)/res/strings.txt)

# -fno-strict-aliasing: as for core (OMKit/Makefile) - file and store records are read through pointers of other types
CFLAGS := -Os -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-strict-aliasing -nostdlib -nostartfiles \
          -Wall -Wextra -Werror -I../sdk/include -I$(GEN) $(KIND_CFLAGS) $(EXTRA)
SRCS := $(wildcard $(MODDIR)/*.c) ../sdk/lib/omk_libc.c
# The module's own headers: a change rebuilds it
HDRS := $(wildcard $(MODDIR)/*.h)

all: $(OUT)/$(UNAME).OMM

$(GEN)/res_strings.h: $(STRINGS) $(TOOLS)/mkmod.py
	@mkdir -p $(GEN)
	$(PYTHON) $(TOOLS)/mkmod.py header "$(STRINGS)" $@

# The kind the module of OUT was built as: another kind into the same folder compiles it again
KIND_STAMP := $(OUT)/$(MOD).kind
$(shell mkdir -p $(OUT) && (echo '$(BUILD)' | cmp -s - $(KIND_STAMP) || echo '$(BUILD)' > $(KIND_STAMP)))

$(OUT)/$(MOD).elf: $(SRCS) $(HDRS) module.ld ../sdk/include/*.h $(GEN)/res_strings.h $(KIND_STAMP) | toolchain-check
	@mkdir -p $(OUT)
	$(TC)gcc $(CFLAGS) $(SRCS) -Wl,-q -Wl,-T,module.ld -Wl,-Map,$(OUT)/$(MOD).map -o $@

# Pictures kept in OMKit/resources/modules/<mod>/icons: read from the card on demand, as <mod>/res/disk. They are committed
# and made by hand with tools/mkicons_film.py (FILMLOOK) and tools/mkicons_creatprf.py (CREATPRF), e.g.
#   python3 OMKit/tools/mkicons_creatprf.py OMKit/resources/modules/creatprf/icons
RES_DISK := $(wildcard $(RESOURCES)/$(MOD)/icons)

# RES_OVER: a folder of resources (laid out as res/: <type>_<id>.* in memory, disk/ on demand) that replace the module's
# own ones of the same type and id - for private experiments only (e.g. trying other pictures without changing the tree);
# no module needs it, and the card bundle is always built without it. Off unless given:
#   make -C OMKit/src/modules MOD=creatprf RES_OVER=/abs/path/to/folder OUT=/abs/path/to/out
RES_OVER ?=

RES_ARGS := --res $(MODDIR)/res $(if $(RES_DISK),--res-disk $(RES_DISK)) $(if $(RES_OVER),--res-over $(RES_OVER))
# The resource options of the last build: the module is built again when they change (RES_OVER given or dropped)
RES_STAMP := $(OUT)/$(MOD).resargs
$(shell mkdir -p $(OUT) && (echo '$(VERSION) $(RES_ARGS)' | cmp -s - $(RES_STAMP) || echo '$(VERSION) $(RES_ARGS)' > $(RES_STAMP)))

$(OUT)/$(UNAME).OMM: $(OUT)/$(MOD).elf $(TOOLS)/mkmod.py $(RES_STAMP) $(wildcard $(MODDIR)/res/* $(MODDIR)/res/disk/*) $(if $(RES_DISK),$(wildcard $(RES_DISK)/*)) $(if $(RES_OVER),$(wildcard $(RES_OVER)/* $(RES_OVER)/disk/*))
	$(PYTHON) $(TOOLS)/mkmod.py build $(OUT)/$(MOD).elf $@ --name $(UNAME) --version $(VERSION) $(RES_ARGS)

# The MN10300 toolchain (toolchain/build-toolchain.sh; another prefix: TC=<prefix>/bin/mn10300-elf-)
toolchain-check:
	@command -v $(TC)gcc >/dev/null 2>&1 || { echo "OMKit: no MN10300 compiler $(TC)gcc: build it with toolchain/build-toolchain.sh, or pass TC=<prefix>/bin/mn10300-elf- (docs/build.md)" >&2; exit 1; }

clean:
	rm -rf $(OUT)/$(MOD).elf $(OUT)/$(MOD).map $(OUT)/$(UNAME).OMM $(RES_STAMP) $(KIND_STAMP) $(GEN)

.PHONY: all clean toolchain-check
