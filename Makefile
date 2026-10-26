CFLAGS = -std=c99 -pedantic -O2 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wstrict-prototypes
BUILD = build

VM = $(BUILD)/terrarium
ASM = $(VM) $(BUILD)/asm.bin
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

RUNTIME = system/lib/rt.asm
# The modules programs import, like io and gfx.
LIBRARIES = $(sort $(wildcard system/lib/*.tr))
COMPILE = $(TC) -L system/lib
TC_SOURCE = $(sort $(wildcard system/tc/*.tr))
KERNEL_SOURCE = $(sort $(wildcard system/kernel/*.tr))
TOOLS = $(BUILD)/asm.bin $(BUILD)/tc.bin
# The assembler and compiler in C, which build the real ones from nothing.
ASM0 = $(BUILD)/asm0
TC0 = $(BUILD)/tc0

PROGRAMS = init sh ls cat echo mkdir rm cp ps kill mem sleep wc term demo edit cube view fractal monitor launch files cmp manifest hold trace piano fetch serve worm calc date clock paint search sort hex life mv play chat
PROGRAM_BINS = $(PROGRAMS:%=$(BUILD)/bin/%.bin)

# What goes on the disk, as host-file:/disk/path.
DISK_FILES = $(foreach p,$(PROGRAMS),$(BUILD)/bin/$(p).bin:/bin/$(p)) \
	$(BUILD)/asm.bin:/bin/asm $(BUILD)/tc.bin:/bin/tc $(BUILD)/terra.tfn:/system/font \
	system/lib/rt.asm:/lib/rt.asm $(foreach f,$(LIBRARIES),$(f):/lib/$(notdir $(f))) \
	examples/hello.tr:/src/hello.tr examples/hello.asm:/src/hello.asm \
	$(foreach f,$(wildcard spec/*.md),$(f):/docs/$(notdir $(f))) README.md:/docs/README.md \
	$(foreach f,$(SOURCES),$(f):/src/$(patsubst system/%,%,$(f)))

# The system's own source goes on the disk too, so Terrarium can rebuild it.
SOURCES = $(wildcard system/tc/*.tr system/kernel/*.tr system/kernel/*.asm system/bin/*.tr system/tools/*.tr \
	system/asm/*.asm system/fonts/*.txt) system/scripts/build-tc system/scripts/build-asm

.PHONY: all test run clean FORCE

all: $(VM) $(TOOLS) $(BUILD)/disk.img $(BUILD)/mkimage.bin $(BUILD)/mksound.bin

# Changing between a build with SDL and one without rebuilds the machine.
$(BUILD)/vm.flags: FORCE
	@mkdir -p $(BUILD)
	@echo '$(SDL_CFLAGS) $(SDL_LIBS)' | cmp -s - $@ || echo '$(SDL_CFLAGS) $(SDL_LIBS)' > $@

$(VM): $(MACHINE) src/machine.h src/host.h src/gpu.h src/gui.h src/audio.h src/net.h $(BUILD)/vm.flags
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $(MACHINE) $(SDL_LIBS)

$(ASM0): bootstrap/asm.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ bootstrap/asm.c

$(TC0): $(wildcard bootstrap/tc/*.c) bootstrap/tc/tc.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(wildcard bootstrap/tc/*.c)

# The C tools build each Terrarium tool from its source, then that result
# builds it again, so the tools you run are always built by themselves.
$(BUILD)/asm.bin: system/asm/asm.asm $(ASM0) $(VM)
	$(ASM0) -o $(BUILD)/asm-stage1.bin system/asm/asm.asm
	$(VM) $(BUILD)/asm-stage1.bin -o $@ -s $(BUILD)/asm.sym system/asm/asm.asm

$(BUILD)/tc.bin: $(TC_SOURCE) $(LIBRARIES) $(RUNTIME) $(TC0) $(BUILD)/asm.bin
	$(TC0) -L system/lib -o $(BUILD)/tc-stage1.asm $(TC_SOURCE)
	$(ASM) -o $(BUILD)/tc-stage1.bin $(RUNTIME) $(BUILD)/tc-stage1.asm
	$(VM) $(BUILD)/tc-stage1.bin -L system/lib -o $(BUILD)/tc.asm $(TC_SOURCE)
	$(ASM) -o $@ -s $(BUILD)/tc.sym $(RUNTIME) $(BUILD)/tc.asm

$(BUILD)/kernel.bin: $(KERNEL_SOURCE) $(LIBRARIES) system/kernel/krt.asm $(TOOLS)
	$(COMPILE) -o $(BUILD)/kernel.asm $(KERNEL_SOURCE)
	$(ASM) -o $@ -s $(BUILD)/kernel.sym system/kernel/krt.asm $(BUILD)/kernel.asm

$(BUILD)/bin/%.bin: system/bin/%.tr $(LIBRARIES) $(RUNTIME) $(TOOLS)
	@mkdir -p $(BUILD)/bin
	$(COMPILE) -o $(BUILD)/bin/$*.asm $<
	$(ASM) -o $@ -s $(BUILD)/bin/$*.sym $(RUNTIME) $(BUILD)/bin/$*.asm

# The host tools: mkdisk, mkfont, mkimage and mksound.
$(BUILD)/mk%.bin: system/tools/mk%.tr $(LIBRARIES) $(RUNTIME) $(TOOLS)
	$(COMPILE) -o $(BUILD)/mk$*.asm $<
	$(ASM) -o $@ $(RUNTIME) $(BUILD)/mk$*.asm

$(BUILD)/terra.tfn: system/fonts/terra.txt $(BUILD)/mkfont.bin
	$(VM) $(BUILD)/mkfont.bin -o $@ system/fonts/terra.txt

$(BUILD)/disk.img: $(BUILD)/mkdisk.bin $(BUILD)/terra.tfn $(BUILD)/kernel.bin $(PROGRAM_BINS) $(TOOLS) $(LIBRARIES) $(RUNTIME) examples/hello.tr $(SOURCES) $(wildcard spec/*.md) README.md
	$(VM) $(BUILD)/mkdisk.bin -o $@ -k $(BUILD)/kernel.bin $(DISK_FILES)

# Terrarium runs on a copy of the disk, so what you do in it lasts until the
# disk image is rebuilt.
$(BUILD)/run.img: $(BUILD)/disk.img
	cp $< $@

# The machine fills the screen; make run WINDOW=1 opens a window instead.
# Built without SDL, it runs in this terminal.
run: all $(BUILD)/run.img
	$(VM) boot $(if $(HEADLESS),--headless,$(if $(WINDOW),,--fullscreen)) $(BUILD)/run.img $(BUILD)/kernel.sym

test: all $(ASM0) $(TC0)
	tests/run.sh

clean:
	rm -rf $(BUILD)
