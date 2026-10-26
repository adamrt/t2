CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wstrict-prototypes
BUILD = build

VM = $(BUILD)/terrarium
ASM = $(VM) $(BUILD)/asm.bin
TC = $(VM) $(BUILD)/tc.bin
MACHINE = src/machine.c src/disasm.c src/monitor.c

RUNTIME = system/lib/rt.asm
LIBRARY = system/lib/core.tr
TC_SOURCE = $(LIBRARY) $(wildcard system/tc/*.tr)

.PHONY: all test seed clean

all: $(VM) $(BUILD)/asm.bin $(BUILD)/tc.bin

$(VM): $(MACHINE) src/machine.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(MACHINE)

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
