CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wstrict-prototypes
BUILD = build

VM = $(BUILD)/terrarium
ASM = $(VM) $(BUILD)/asm.bin
MACHINE = src/machine.c src/disasm.c src/monitor.c

.PHONY: all test seed clean

all: $(VM) $(BUILD)/asm.bin

$(VM): $(MACHINE) src/machine.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(MACHINE)

# The seed assembles the current source, and that result assembles it again,
# so the final assembler is always built by itself.
$(BUILD)/asm.bin: system/asm/asm.asm bootstrap/asm.bin $(VM)
	$(VM) bootstrap/asm.bin -o $(BUILD)/asm-stage1.bin system/asm/asm.asm
	$(VM) $(BUILD)/asm-stage1.bin -o $@ -s $(BUILD)/asm.sym system/asm/asm.asm

test: all
	tests/run.sh $(ASM)

# Replaces the seeds after checking that each tool reproduces itself.
seed: all
	$(ASM) -o $(BUILD)/asm-check.bin system/asm/asm.asm
	cmp $(BUILD)/asm.bin $(BUILD)/asm-check.bin
	cp $(BUILD)/asm.bin bootstrap/asm.bin

clean:
	rm -rf $(BUILD)
