CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wstrict-prototypes
BUILD = build

VM = $(BUILD)/terrarium
ASM = $(VM) $(BUILD)/asm.bin
TC = $(VM) $(BUILD)/tc.bin
MACHINE = src/machine.c src/disasm.c src/host.c src/monitor.c src/boot.c src/gpu.c src/gui.c
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS := $(shell pkg-config --libs sdl3)

RUNTIME = system/lib/rt.asm
LIBRARY = system/lib/core.tr
OS_LIBRARY = $(LIBRARY) system/lib/os.tr system/lib/gfx.tr
TC_SOURCE = $(LIBRARY) $(wildcard system/tc/*.tr)
KERNEL_SOURCE = $(LIBRARY) $(wildcard system/kernel/*.tr)
TOOLS = $(BUILD)/asm.bin $(BUILD)/tc.bin

PROGRAMS = init sh ls cat echo mkdir rm cp ps kill mem sleep wc term demo
PROGRAM_BINS = $(PROGRAMS:%=$(BUILD)/bin/%.bin)

# What goes on the disk, as host-file:/disk/path.
DISK_FILES = $(foreach p,$(PROGRAMS),$(BUILD)/bin/$(p).bin:/bin/$(p)) \
	$(BUILD)/asm.bin:/bin/asm $(BUILD)/tc.bin:/bin/tc $(BUILD)/terra.tfn:/system/font \
	system/lib/rt.asm:/lib/rt.asm system/lib/core.tr:/lib/core.tr system/lib/os.tr:/lib/os.tr \
	examples/hello.tr:/src/hello.tr examples/hello.asm:/src/hello.asm

.PHONY: all test seed run clean

all: $(VM) $(TOOLS) $(BUILD)/disk.img

$(VM): $(MACHINE) src/machine.h src/host.h src/gpu.h src/gui.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $(MACHINE) $(SDL_LIBS)

# Each tool is built by its seed, then rebuilt by that result, so the tools
# you run are always built by themselves.
$(BUILD)/asm.bin: system/asm/asm.asm bootstrap/asm.bin $(VM)
	$(VM) bootstrap/asm.bin -o $(BUILD)/asm-stage1.bin system/asm/asm.asm
	$(VM) $(BUILD)/asm-stage1.bin -o $@ -s $(BUILD)/asm.sym system/asm/asm.asm

$(BUILD)/tc.bin: $(TC_SOURCE) $(RUNTIME) bootstrap/tc.bin $(BUILD)/asm.bin
	$(VM) bootstrap/tc.bin -o $(BUILD)/tc-stage1.asm $(TC_SOURCE)
	$(ASM) -o $(BUILD)/tc-stage1.bin $(RUNTIME) $(BUILD)/tc-stage1.asm
	$(VM) $(BUILD)/tc-stage1.bin -o $(BUILD)/tc.asm $(TC_SOURCE)
	$(ASM) -o $@ -s $(BUILD)/tc.sym $(RUNTIME) $(BUILD)/tc.asm

$(BUILD)/kernel.bin: $(KERNEL_SOURCE) system/kernel/krt.asm $(TOOLS)
	$(TC) -o $(BUILD)/kernel.asm $(KERNEL_SOURCE)
	$(ASM) -o $@ -s $(BUILD)/kernel.sym system/kernel/krt.asm $(BUILD)/kernel.asm

$(BUILD)/bin/%.bin: system/bin/%.tr $(OS_LIBRARY) $(RUNTIME) $(TOOLS)
	@mkdir -p $(BUILD)/bin
	$(TC) -o $(BUILD)/bin/$*.asm $(OS_LIBRARY) $<
	$(ASM) -o $@ -s $(BUILD)/bin/$*.sym $(RUNTIME) $(BUILD)/bin/$*.asm

$(BUILD)/mkdisk.bin: system/tools/mkdisk.tr $(LIBRARY) $(RUNTIME) $(TOOLS)
	$(TC) -o $(BUILD)/mkdisk.asm $(LIBRARY) system/tools/mkdisk.tr
	$(ASM) -o $@ $(RUNTIME) $(BUILD)/mkdisk.asm

$(BUILD)/mkfont.bin: system/tools/mkfont.tr $(LIBRARY) $(RUNTIME) $(TOOLS)
	$(TC) -o $(BUILD)/mkfont.asm $(LIBRARY) system/tools/mkfont.tr
	$(ASM) -o $@ $(RUNTIME) $(BUILD)/mkfont.asm

$(BUILD)/terra.tfn: system/fonts/terra.txt $(BUILD)/mkfont.bin
	$(VM) $(BUILD)/mkfont.bin -o $@ system/fonts/terra.txt

$(BUILD)/disk.img: $(BUILD)/mkdisk.bin $(BUILD)/terra.tfn $(BUILD)/kernel.bin $(PROGRAM_BINS) $(TOOLS) $(OS_LIBRARY) $(RUNTIME) examples/hello.tr
	$(VM) $(BUILD)/mkdisk.bin -o $@ -k $(BUILD)/kernel.bin $(DISK_FILES)

# Terrarium runs on a copy of the disk, so what you do in it lasts until the
# disk image is rebuilt.
$(BUILD)/run.img: $(BUILD)/disk.img
	cp $< $@

run: all $(BUILD)/run.img
	$(VM) boot $(BUILD)/run.img $(BUILD)/kernel.sym

test: all
	tests/run.sh

# Replaces the seeds after checking that each tool reproduces itself.
seed: all
	$(ASM) -o $(BUILD)/asm-check.bin system/asm/asm.asm
	cmp $(BUILD)/asm.bin $(BUILD)/asm-check.bin
	$(TC) -o $(BUILD)/tc-check.asm $(TC_SOURCE)
	cmp $(BUILD)/tc.asm $(BUILD)/tc-check.asm
	cp $(BUILD)/asm.bin bootstrap/asm.bin
	cp $(BUILD)/tc.bin bootstrap/tc.bin

clean:
	rm -rf $(BUILD)
