CFLAGS = -std=c11 -O2 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wstrict-prototypes
BUILD = build

MACHINE = src/machine.c src/disasm.c src/monitor.c

.PHONY: all clean

all: $(BUILD)/terrarium

$(BUILD)/terrarium: $(MACHINE) src/machine.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(MACHINE)

clean:
	rm -rf $(BUILD)
