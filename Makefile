CFLAGS = -std=c99 -pedantic -O2 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wstrict-prototypes
BUILD = build

VM = $(BUILD)/terrarium
TC = $(VM) $(BUILD)/tc.bin
MACHINE = src/machine.c src/disasm.c src/host.c src/monitor.c src/boot.c src/gpu.c src/gui.c src/audio.c src/net.c
# SDL3 gives the machine a window and sound. Without it, or with
# make HEADLESS=1, everything still builds, and the machine runs with
# --headless or --screen.
ifeq ($(HEADLESS),)
SDL_LIBS := $(shell pkg-config --libs sdl3 2>/dev/null)
SDL_CFLAGS := $(shell pkg-config --cflags sdl3 2>/dev/null)
ifeq ($(SDL_LIBS),)
HEADLESS = 1
endif
endif
ifneq ($(HEADLESS),)
SDL_CFLAGS = -DHEADLESS
SDL_LIBS =
endif

RUNTIME = system/lib/runtime.asm
# The modules programs import, like io and gfx.
LIBRARIES = $(sort $(wildcard system/lib/*.tr))
COMPILE = $(TC) -L system/lib
TC_SOURCE = $(sort $(wildcard system/tc/*.tr))
KERNEL_SOURCE = $(sort $(wildcard system/kernel/*.tr))
# The compiler in C, which builds the real one from nothing.
TC0 = $(BUILD)/tc0

PROGRAMS = init sh ls cat echo mkdir rm cp ps kill mem sleep wc term demo edit cube view fractal monitor launch files cmp manifest hold trace piano fetch serve worm calc date clock paint search sort hex life mv play chat
PROGRAM_BINS = $(PROGRAMS:%=$(BUILD)/bin/%.bin)

# What goes on the disk, as host-file:/disk/path.
DISK_FILES = $(foreach p,$(PROGRAMS),$(BUILD)/bin/$(p).bin:/bin/$(p)) \
	$(BUILD)/tc.bin:/bin/tc $(BUILD)/terra.tfn:/system/font \
	$(RUNTIME):/lib/runtime.asm $(foreach f,$(LIBRARIES),$(f):/lib/$(notdir $(f))) \
	examples/hello.tr:/src/hello.tr examples/hello.asm:/src/hello.asm \
	$(foreach f,$(wildcard docs/*.md),$(f):/docs/$(notdir $(f))) README.md:/docs/README.md \
	$(foreach f,$(SOURCES),$(f):/src/$(patsubst system/%,%,$(f)))

# The system's own source goes on the disk too, so Terrarium can rebuild it.
SOURCES = $(wildcard system/tc/*.tr system/kernel/*.tr system/kernel/*.asm system/bin/*.tr system/tools/*.tr \
	system/fonts/*.txt) system/scripts/build-tc

.PHONY: all test run clean FORCE

all: $(VM) $(BUILD)/tc.bin $(BUILD)/disk.img $(BUILD)/mkimage.bin $(BUILD)/mksound.bin

# Changing between a build with SDL and one without rebuilds the machine.
$(BUILD)/vm.flags: FORCE
	@mkdir -p $(BUILD)
	@echo '$(SDL_CFLAGS) $(SDL_LIBS)' | cmp -s - $@ || echo '$(SDL_CFLAGS) $(SDL_LIBS)' > $@

$(VM): $(MACHINE) src/machine.h src/host.h src/gpu.h src/gui.h src/audio.h src/net.h $(BUILD)/vm.flags
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $(MACHINE) $(SDL_LIBS)

$(TC0): $(wildcard bootstrap/tc/*.c) bootstrap/tc/tc.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(wildcard bootstrap/tc/*.c)

# The C compiler builds the Terrarium one from its source, then that result
# builds itself again, so the compiler you run is always built by itself.
$(BUILD)/tc.bin: $(TC_SOURCE) $(LIBRARIES) $(RUNTIME) $(TC0) $(VM)
	$(TC0) -L system/lib -o $(BUILD)/tc-stage1.bin $(TC_SOURCE)
	$(VM) $(BUILD)/tc-stage1.bin -L system/lib -o $@ -s $(BUILD)/tc.sym $(TC_SOURCE)

$(BUILD)/kernel.bin: $(KERNEL_SOURCE) $(LIBRARIES) system/kernel/kruntime.asm $(BUILD)/tc.bin
	$(COMPILE) -r system/kernel/kruntime.asm -o $@ -s $(BUILD)/kernel.sym $(KERNEL_SOURCE)

$(BUILD)/bin/%.bin: system/bin/%.tr $(LIBRARIES) $(RUNTIME) $(BUILD)/tc.bin
	@mkdir -p $(BUILD)/bin
	$(COMPILE) -o $@ -s $(BUILD)/bin/$*.sym $<

# The host tools: mkdisk, mkfont, mkimage and mksound.
$(BUILD)/mk%.bin: system/tools/mk%.tr $(LIBRARIES) $(RUNTIME) $(BUILD)/tc.bin
	$(COMPILE) -o $@ $<

$(BUILD)/terra.tfn: system/fonts/terra.txt $(BUILD)/mkfont.bin
	$(VM) $(BUILD)/mkfont.bin -o $@ system/fonts/terra.txt

$(BUILD)/disk.img: $(BUILD)/mkdisk.bin $(BUILD)/terra.tfn $(BUILD)/kernel.bin $(PROGRAM_BINS) $(BUILD)/tc.bin $(LIBRARIES) $(RUNTIME) examples/hello.tr $(SOURCES) $(wildcard docs/*.md) README.md
	$(VM) $(BUILD)/mkdisk.bin -o $@ -k $(BUILD)/kernel.bin $(DISK_FILES)

# Terrarium runs on a copy of the disk, so what you do in it lasts until the
# disk image is rebuilt.
$(BUILD)/run.img: $(BUILD)/disk.img
	cp $< $@

# The machine fills the screen; make run WINDOW=1 opens a window instead.
# Built without SDL, it runs in this terminal.
run: all $(BUILD)/run.img
	$(VM) boot $(if $(HEADLESS),--headless,$(if $(WINDOW),,--fullscreen)) $(BUILD)/run.img $(BUILD)/kernel.sym

test: all $(TC0)
	tests/run.sh

clean:
	rm -rf $(BUILD)
