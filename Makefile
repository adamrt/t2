CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wstrict-prototypes
BUILD = build

VM = $(BUILD)/terrarium
ASM = $(VM) $(BUILD)/asm.bin
TC = $(VM) $(BUILD)/tc.bin
MACHINE = src/machine.c src/disasm.c src/host.c src/monitor.c src/boot.c src/gpu.c src/gui.c src/audio.c src/net.c
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS := $(shell pkg-config --libs sdl3)

RUNTIME = system/lib/rt.asm
# The modules programs import, like io and gfx.
LIBRARIES = $(sort $(wildcard system/lib/*.tr))
COMPILE = $(TC) -L system/lib
TC_SOURCE = $(sort $(wildcard system/tc/*.tr))
KERNEL_SOURCE = $(sort $(wildcard system/kernel/*.tr))
TOOLS = $(BUILD)/asm.bin $(BUILD)/tc.bin

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

.PHONY: all test seed run clean

all: $(VM) $(TOOLS) $(BUILD)/disk.img $(BUILD)/mkimage.bin $(BUILD)/mksound.bin

$(VM): $(MACHINE) src/machine.h src/host.h src/gpu.h src/gui.h src/audio.h src/net.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $(MACHINE) $(SDL_LIBS)

# Each tool is built by its seed, then rebuilt by that result, so the tools
# you run are always built by themselves.
$(BUILD)/asm.bin: system/asm/asm.asm bootstrap/asm.bin $(VM)
	$(VM) bootstrap/asm.bin -o $(BUILD)/asm-stage1.bin system/asm/asm.asm
	$(VM) $(BUILD)/asm-stage1.bin -o $@ -s $(BUILD)/asm.sym system/asm/asm.asm

$(BUILD)/tc.bin: $(TC_SOURCE) $(LIBRARIES) $(RUNTIME) bootstrap/tc.bin $(BUILD)/asm.bin
	$(VM) bootstrap/tc.bin -L system/lib -o $(BUILD)/tc-stage1.asm $(TC_SOURCE)
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
run: all $(BUILD)/run.img
	$(VM) boot $(if $(WINDOW),,--fullscreen) $(BUILD)/run.img $(BUILD)/kernel.sym

test: all
	tests/run.sh

# Replaces the seeds after checking that each tool reproduces itself.
seed: all
	$(ASM) -o $(BUILD)/asm-check.bin system/asm/asm.asm
	cmp $(BUILD)/asm.bin $(BUILD)/asm-check.bin
	$(COMPILE) -o $(BUILD)/tc-check.asm $(TC_SOURCE)
	cmp $(BUILD)/tc.asm $(BUILD)/tc-check.asm
	cp $(BUILD)/asm.bin bootstrap/asm.bin
	cp $(BUILD)/tc.bin bootstrap/tc.bin

clean:
	rm -rf $(BUILD)
