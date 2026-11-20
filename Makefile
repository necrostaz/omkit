# OMKit build (Linux or WSL; docs/build.md):
#   make -C OMKit          core             -> OMKit/build/CORE.BIN
#   make -C OMKit card     the card bundle  -> OMKit/build/card/OMKIT/
#   make -C OMKit test     host tests, data checks, the apps' tests
#   make -C OMKit clean    removes OUT
# OUT=<dir> builds elsewhere (relative to this folder, or absolute); TC=<prefix>/bin/mn10300-elf- takes a toolchain
# installed elsewhere than ~/opt/mn10300 (toolchain/build-toolchain.sh).
PLATFORM := om5m2/fw1100
OUT ?= build
TC := $(HOME)/opt/mn10300/bin/mn10300-elf-
PYTHON ?= python3

# -fno-strict-aliasing: the code reads records, headers and firmware memory through pointers of other types (u8 buffers
# as u16/u32 words, structures over arena blocks), as embedded code usually does; the type-based alias rule must not
# let the optimizer reorder such reads and writes
CFLAGS := -Os -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-strict-aliasing -nostdlib -Wall -Wextra -Isrc/core/include -Isrc/sdk/include $(EXTRA)
OBJS := $(addprefix $(OUT)/,main.o clock.o mem.o cfg.o log.o store.o util.o module.o event.o api.o power.o prof.o look.o isp.o grain.o taskdump.o drv.o pictprof.o editor.o import.o ispdrv.o graindrv.o kernel.o thunks.o)
LD_SCRIPTS := src/core/core.ld src/platform/$(PLATFORM)/fw.ld

all: $(OUT)/CORE.BIN

# The MN10300 toolchain must be there before anything is compiled for the camera
toolchain-check:
	@command -v $(TC)gcc >/dev/null 2>&1 || { echo "OMKit: no MN10300 compiler $(TC)gcc: build it with toolchain/build-toolchain.sh, or pass TC=<prefix>/bin/mn10300-elf- (docs/build.md)" >&2; exit 1; }

$(OBJS): | toolchain-check

$(OUT)/%.o: src/core/%.c src/core/include/*.h src/sdk/include/*.h
	@mkdir -p $(OUT)
	$(TC)gcc $(CFLAGS) -c $< -o $@

$(OUT)/drv.o: src/platform/$(PLATFORM)/drv.c src/core/include/*.h src/sdk/include/*.h
	@mkdir -p $(OUT)
	$(TC)gcc $(CFLAGS) -c $< -o $@

$(OUT)/pictprof.o: src/platform/$(PLATFORM)/pictprof.c src/core/include/*.h src/sdk/include/*.h
	@mkdir -p $(OUT)
	$(TC)gcc $(CFLAGS) -c $< -o $@

# edtrig.h, texfill.h: plain C parts of the driver that the host tests run as they are (tests/host)
$(OUT)/editor.o: src/platform/$(PLATFORM)/editor.c src/platform/$(PLATFORM)/edtrig.h src/core/include/*.h src/sdk/include/*.h
	@mkdir -p $(OUT)
	$(TC)gcc $(CFLAGS) -c $< -o $@

$(OUT)/import.o: src/platform/$(PLATFORM)/import.c src/core/include/*.h src/sdk/include/*.h
	@mkdir -p $(OUT)
	$(TC)gcc $(CFLAGS) -c $< -o $@

$(OUT)/ispdrv.o: src/platform/$(PLATFORM)/ispdrv.c src/core/include/*.h src/sdk/include/*.h
	@mkdir -p $(OUT)
	$(TC)gcc $(CFLAGS) -c $< -o $@

$(OUT)/graindrv.o: src/platform/$(PLATFORM)/graindrv.c src/platform/$(PLATFORM)/texfill.h src/core/include/*.h src/sdk/include/*.h
	@mkdir -p $(OUT)
	$(TC)gcc $(CFLAGS) -c $< -o $@

$(OUT)/kernel.o: src/platform/$(PLATFORM)/kernel.c src/core/include/*.h src/sdk/include/*.h
	@mkdir -p $(OUT)
	$(TC)gcc $(CFLAGS) -c $< -o $@

$(OUT)/thunks.o: src/platform/$(PLATFORM)/thunks.s
	@mkdir -p $(OUT)
	$(TC)as $< -o $@

$(OUT)/core.elf: $(OBJS) $(LD_SCRIPTS)
	$(TC)ld -T src/platform/$(PLATFORM)/fw.ld -T src/core/core.ld $(OBJS) -o $@ -Map $(OUT)/core.map

$(OUT)/core.body: $(OUT)/core.elf
	$(TC)objcopy -O binary $< $@

$(OUT)/CORE.BIN: $(OUT)/core.body
	$(PYTHON) tools/pack.py $< $@

# Card bundle: <OUT>/card/OMKIT/CORE.BIN, the bundled modules in MODULES (always without RES_OVER: only what OMKit
# itself has, e.g. the committed pictures of resources/), the bundled grain tiles in GRAIN
# (resources/modules/filmlook/grain/manifest.json: tools/grainset.py checks every file's SHA-256 and container before it
# copies) and the bundled FILMLOOK presets in FILMLOOK (resources/modules/filmlook/presets/manifest.json, checked the same
# way by tools/presetset.py), and OMKIT.INI: core's own sample (src/core/cfg.c) with the bundled modules on, made by
# tools/cardini.py, so that the modules run from the first start (core writes its sample, which loads no module,
# only when the card has no OMKIT.INI). RELEASE_MODULES: name:version, in load order
RELEASE_MODULES := creatprf:1.5 filmlook:1.12
CARD := $(OUT)/card/OMKIT

card: $(OUT)/CORE.BIN
	rm -rf $(CARD)
	mkdir -p $(CARD)/MODULES
	cp $(OUT)/CORE.BIN $(CARD)/
	for mv in $(RELEASE_MODULES); do m=$${mv%%:*}; $(MAKE) -C src/modules MOD=$$m VERSION=$${mv#*:} OUT=$(abspath $(CARD))/../build RES_OVER= && 		cp $(CARD)/../build/$$(echo $$m | tr a-z A-Z).OMM $(CARD)/MODULES/ || exit 1; done
	rm -rf $(CARD)/../build
	$(PYTHON) tools/grainset.py copy $(CARD)/GRAIN
	$(PYTHON) tools/presetset.py copy $(CARD)/FILMLOOK
	$(PYTHON) tools/cardini.py write $(CARD)/OMKIT.INI $(RELEASE_MODULES)
	cd $(CARD)/.. && find OMKIT -type f | sort | xargs sha256sum

# Tests (docs/build.md, "Tests"). Host tests: parts of core and of the modules built with the host compiler against the
# stub driver tests/host/host_drv.c and run by tests/host/run.sh; the module loader test takes a real HELLO.OMM, built
# into <OUT>/test/modules with the MN10300 toolchain (skipped without it), as do the tests of tools/mkmod.py
# (tests/test_mkmod.py). Then the data sets and the committed pictures are checked and the apps' tests run; a check
# that needs numpy (or numpy and Pillow) is skipped without them, with a message.
HOST_CC ?= gcc
TEST := $(OUT)/test
HOST_CFLAGS := -std=gnu99 -Wall -Wextra -Werror
HOST_INC := -Isrc/core/include -Isrc/sdk/include -Itests/host
HOST_CORE := $(addprefix src/core/,clock.c mem.c cfg.c log.c store.c util.c power.c prof.c isp.c)
HOST_DEPS := src/core/include/*.h src/sdk/include/*.h tests/host/host_drv.h tests/host/host_drv.c
HAVE_TC = $(shell command -v $(TC)gcc 2>/dev/null)
HAVE_NUMPY = $(shell $(PYTHON) -c "import numpy" 2>/dev/null && echo yes)
HAVE_PIL = $(shell $(PYTHON) -c "import numpy, PIL" 2>/dev/null && echo yes)

$(TEST)/test_core: $(HOST_CORE) tests/host/test_core.c $(HOST_DEPS)
	@mkdir -p $(TEST)
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_INC) $(HOST_CORE) tests/host/host_drv.c tests/host/test_core.c -o $@

$(TEST)/test_mod: $(HOST_CORE) src/core/module.c src/core/api.c src/core/event.c tests/host/test_mod.c $(HOST_DEPS)
	@mkdir -p $(TEST)
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_INC) $(HOST_CORE) src/core/module.c src/core/api.c src/core/event.c tests/host/host_drv.c \
		tests/host/test_mod.c -o $@

$(TEST)/test_ispb: src/modules/filmlook/edits.c src/modules/filmlook/*.h tests/host/test_ispb.c src/sdk/include/*.h
	@mkdir -p $(TEST)
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/sdk/include -Isrc/modules/filmlook src/modules/filmlook/edits.c tests/host/test_ispb.c -o $@

$(TEST)/test_ispfile: src/modules/filmlook/ispfile.c src/modules/filmlook/ispfile.h tests/host/test_ispfile.c src/sdk/include/*.h
	@mkdir -p $(TEST)
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/sdk/include -Isrc/modules/filmlook src/modules/filmlook/ispfile.c tests/host/test_ispfile.c -o $@

FILMLOOK_SRC := src/modules/filmlook
$(TEST)/test_filmlook: $(FILMLOOK_SRC)/*.c $(FILMLOOK_SRC)/*.h tests/host/test_filmlook.c src/sdk/include/*.h
	@mkdir -p $(TEST)
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/sdk/include -I$(FILMLOOK_SRC) $(FILMLOOK_SRC)/edits.c $(FILMLOOK_SRC)/ispfile.c tests/host/test_filmlook.c -o $@

# The driver's plain C parts: the grain texture's fill and the editors' sines
$(TEST)/test_texfill: src/platform/$(PLATFORM)/texfill.h tests/host/test_texfill.c src/sdk/include/omk_types.h
	@mkdir -p $(TEST)
	$(HOST_CC) $(HOST_CFLAGS) -O1 -Isrc/sdk/include -Isrc/platform/$(PLATFORM) tests/host/test_texfill.c -o $@

$(TEST)/test_edtrig: src/platform/$(PLATFORM)/edtrig.h tests/host/test_edtrig.c src/sdk/include/omk_types.h
	@mkdir -p $(TEST)
	$(HOST_CC) $(HOST_CFLAGS) -Isrc/sdk/include -Isrc/platform/$(PLATFORM) tests/host/test_edtrig.c -o $@

# HELLO.OMM and hello.elf for the loader and mkmod tests, when the MN10300 toolchain is there
test-hello:
	$(if $(HAVE_TC),@$(MAKE) -s -C src/modules MOD=hello OUT=$(abspath $(TEST))/modules >/dev/null)

test-host: $(TEST)/test_core $(TEST)/test_mod $(TEST)/test_ispb $(TEST)/test_ispfile $(TEST)/test_filmlook            $(TEST)/test_texfill $(TEST)/test_edtrig test-hello
	@echo "host tests:"
	@sh tests/host/run.sh $(TEST) $(HOST_CC) $(if $(HAVE_TC),$(abspath $(TEST))/modules $(TC) "$(PYTHON)")

test-tools: test-hello
	@echo "tools/mkmod.py:"
	@OMKIT_TEST_MODULES=$(if $(HAVE_TC),$(abspath $(TEST))/modules) $(PYTHON) -m unittest discover -s tests

test-data:
	@echo "data sets and pictures:"
	$(PYTHON) tools/grainset.py check
	$(PYTHON) tools/presetset.py check
	$(PYTHON) tools/mkicons_film.py resources/modules/filmlook/icons --check
	$(if $(HAVE_NUMPY),$(PYTHON) tools/mkicons_creatprf.py resources/modules/creatprf/icons --check,@echo "  skip  mkicons_creatprf.py --check: numpy is not installed for $(PYTHON)")

test-apps:
	@echo "apps:"
	$(if $(HAVE_PIL),$(PYTHON) -m unittest discover -s apps -t apps,@echo "  skip  the apps' tests: numpy and Pillow are not installed for $(PYTHON) (apps/requirements.txt)")

test: test-host test-tools test-data test-apps
	@echo "make test: all passed"

clean:
	rm -rf $(OUT)

.PHONY: all clean card toolchain-check test test-hello test-host test-tools test-data test-apps
