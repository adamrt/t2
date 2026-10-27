// Helpers shared by the development monitor and the full machine.

#define _POSIX_C_SOURCE 200809L

#include "host.h"
#include "bytes.h"

#include <limits.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

struct Symbol {
    uint32_t address;
    char name[64];
};

uint32_t
align4(uint32_t n)
{
    return (n + 3) & ~3u;
}

uint8_t *
read_file(const char *path, size_t *size)
{
    *size = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    uint8_t *buffer = NULL;
    if (fseek(file, 0, SEEK_END) != 0)
        goto cleanup;
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0)
        goto cleanup;
    buffer = malloc(length > 0 ? (size_t)length : 1);
    if (buffer != NULL && fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        free(buffer);
        buffer = NULL;
    }
    if (buffer != NULL)
        *size = (size_t)length;
cleanup:
    fclose(file);
    return buffer;
}

const char *
parse_executable(const uint8_t *file, size_t size, Executable *exe)
{
    if (size < EXECUTABLE_HEADER_SIZE || memcmp(file, "TRX2", 4) != 0)
        return "not an executable";
    exe->permissions = read_u32_le(file + 28);
    exe->entry_pc = read_u32_le(file + 4);
    exe->code_size = read_u32_le(file + 8);
    exe->data_size = read_u32_le(file + 12);
    exe->bss_size = read_u32_le(file + 16);
    exe->stack_size = read_u32_le(file + 20);
    exe->memory_size = read_u32_le(file + 24);
    if (exe->code_size % 4 != 0 || exe->memory_size % 4 != 0 ||
        (uint64_t)EXECUTABLE_HEADER_SIZE + exe->code_size + exe->data_size > size)
        return "malformed header";
    // Check sizes before rounding them to prevent alignment from wrapping.
    if (exe->memory_size > RAM_SIZE || exe->data_size > RAM_SIZE || exe->bss_size > RAM_SIZE ||
        (uint64_t)exe->code_size + align4(exe->data_size) + align4(exe->bss_size) +
                exe->stack_size >
            exe->memory_size)
        return "doesn't fit in its declared memory";
    if (exe->entry_pc < NULL_GUARD || exe->entry_pc - NULL_GUARD >= exe->code_size ||
        exe->entry_pc % 4 != 0)
        return "bad entry point";
    exe->code_bytes = file + EXECUTABLE_HEADER_SIZE;
    exe->data_bytes = exe->code_bytes + exe->code_size;
    return NULL;
}

void
symbols_free(SymbolTable *table)
{
    free(table->items);
    table->items = NULL;
    table->count = 0;
}

// Symbols come from an optional "name.sym" file next to "name.bin", with
// lines of the form "c 00010000 start". Only code symbols are used.
void
symbols_load(SymbolTable *table, const char *program)
{
    symbols_free(table);
    char path[1024];
    snprintf(path, sizeof(path), "%s", program);
    char *dot = strrchr(path, '.');
    if (dot == NULL || strchr(dot, '/') != NULL)
        dot = path + strlen(path);
    snprintf(dot, sizeof(path) - (size_t)(dot - path), ".sym");

    FILE *file = fopen(path, "r");
    if (file == NULL)
        return;
    char line[256];
    size_t capacity = 0;
    while (fgets(line, sizeof(line), file)) {
        Symbol symbol;
        char kind;
        if (sscanf(line, "%c %x %63s", &kind, &symbol.address, symbol.name) != 3 || kind != 'c')
            continue;
        // Compiler-made labels like L12 say nothing about where you are.
        if (symbol.name[0] == 'L' && symbol.name[1] >= '0' && symbol.name[1] <= '9')
            continue;
        if (table->count == capacity) {
            capacity = capacity ? capacity * 2 : 256;
            Symbol *items = realloc(table->items, capacity * sizeof(*items));
            if (items == NULL)
                break;
            table->items = items;
        }
        table->items[table->count++] = symbol;
    }
    fclose(file);
}

static const char *
symbol_for(const SymbolTable *table, uint32_t address, uint32_t *offset)
{
    const Symbol *best = NULL;
    for (size_t i = 0; i < table->count; i++) {
        if (table->items[i].address <= address &&
            (best == NULL || table->items[i].address > best->address))
            best = &table->items[i];
    }
    if (best == NULL)
        return NULL;
    *offset = address - best->address;
    return best->name;
}

void
print_location(const SymbolTable *table, uint32_t address)
{
    uint32_t offset;
    const char *name = symbol_for(table, address, &offset);
    fprintf(stderr, "0x%08x", address);
    if (name != NULL)
        fprintf(stderr, " %s+%u", name, offset);
}

void
report_fault(Machine *machine, const SymbolTable *symbols)
{
    char text[96] = "?";
    uint32_t instruction;
    if (machine_fetch(machine, machine->fault_pc, &instruction))
        disasm(instruction, machine->fault_pc, text, sizeof(text));

    fprintf(stderr, "fault: %s at ", fault_name(machine->fault));
    print_location(symbols, machine->fault_pc);
    fprintf(stderr, ": %s\n", text);
    if (machine->fault == FAULT_DATA_ADDRESS || machine->fault == FAULT_ALIGNMENT ||
        machine->fault == FAULT_STACK || machine->fault == FAULT_PORT)
        fprintf(stderr, "  address 0x%08x\n", machine->fault_addr);
    static const char *names[REG_COUNT] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "sp" };
    for (int i = 0; i < REG_COUNT; i++)
        fprintf(stderr, "  %s %08x%s", names[i], machine->r[i], i % 3 == 2 ? "\n" : "");

    // Anything on the stack that looks like a code address is probably a
    // return address.
    fprintf(stderr, "  stack:\n");
    int shown = 0;
    for (uint32_t sp = machine->r[REG_SP]; shown < 12; sp += 4) {
        const uint8_t *p = machine_data(machine, sp, 4);
        if (p == NULL)
            break;
        uint32_t v = read_u32_le(p);
        if (v % 4 == 0 && v >= NULL_GUARD + 4 && v - NULL_GUARD <= machine->space.code.size) {
            fprintf(stderr, "    ");
            print_location(symbols, v - 4);
            fprintf(stderr, "\n");
            shown++;
        }
    }
}

uint64_t
host_monotonic_us(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000 + (uint64_t)time.tv_nsec / 1000;
}

int
host_deadline_ms(uint64_t deadline_us, uint64_t now_us)
{
    if (deadline_us == 0)
        return -1;
    if (deadline_us <= now_us)
        return 0;
    uint64_t remaining_us = deadline_us - now_us;
    uint64_t milliseconds = remaining_us / 1000 + (remaining_us % 1000 != 0);
    return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

int
host_min_timeout(int first_ms, int second_ms)
{
    if (first_ms < 0)
        return second_ms;
    if (second_ms < 0)
        return first_ms;
    return first_ms < second_ms ? first_ms : second_ms;
}
