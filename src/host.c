// Helpers shared by the development monitor and the full machine.

#define _POSIX_C_SOURCE 200809L

#include "host.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t addr;
    char name[64];
} Symbol;

static Symbol *symbols;
static size_t symbol_count;

uint32_t
read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t
align4(uint32_t n)
{
    return (n + 3) & ~3u;
}

uint8_t *
read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    if (buf != NULL && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

const char *
parse_executable(const uint8_t *file, size_t size, Executable *exe)
{
    // TRX1 is TRX2 without the permissions word; the seeds still use it.
    if (size >= 28 && memcmp(file, "TRX1", 4) == 0) {
        exe->header_size = 28;
        exe->permissions = 0xffffffffu;
    } else if (size >= 32 && memcmp(file, "TRX2", 4) == 0) {
        exe->header_size = 32;
        exe->permissions = read32(file + 28);
    } else {
        return "not an executable";
    }
    exe->entry = read32(file + 4);
    exe->code = read32(file + 8);
    exe->data = read32(file + 12);
    exe->bss = read32(file + 16);
    exe->stack = read32(file + 20);
    exe->memory = read32(file + 24);
    exe->code_bytes = file + exe->header_size;
    exe->data_bytes = file + exe->header_size + exe->code;
    if (exe->code % 4 != 0 || exe->memory % 4 != 0 || (uint64_t)exe->header_size + exe->code + exe->data > size)
        return "malformed header";
    if (exe->memory > RAM_SIZE || (uint64_t)exe->code + align4(exe->data) + align4(exe->bss) + exe->stack > exe->memory)
        return "doesn't fit in its declared memory";
    if (exe->entry < NULL_GUARD || exe->entry - NULL_GUARD >= exe->code || exe->entry % 4 != 0)
        return "bad entry point";
    return NULL;
}

// Symbols come from an optional "name.sym" file next to "name.bin", with
// lines of the form "c 00010000 start". Only code symbols are used.
void
load_symbols(const char *program)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s", program);
    char *dot = strrchr(path, '.');
    if (dot == NULL || strchr(dot, '/') != NULL)
        dot = path + strlen(path);
    snprintf(dot, sizeof(path) - (size_t)(dot - path), ".sym");

    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    char line[256];
    size_t cap = 0;
    while (fgets(line, sizeof(line), f)) {
        Symbol s;
        char kind;
        if (sscanf(line, "%c %x %63s", &kind, &s.addr, s.name) != 3 || kind != 'c')
            continue;
        // Compiler-made labels like L12 say nothing about where you are.
        if (s.name[0] == 'L' && s.name[1] >= '0' && s.name[1] <= '9')
            continue;
        if (symbol_count == cap) {
            cap = cap ? cap * 2 : 256;
            symbols = realloc(symbols, cap * sizeof(Symbol));
        }
        symbols[symbol_count++] = s;
    }
    fclose(f);
}

static const char *
symbol_for(uint32_t addr, uint32_t *offset)
{
    const Symbol *best = NULL;
    for (size_t i = 0; i < symbol_count; i++) {
        if (symbols[i].addr <= addr && (best == NULL || symbols[i].addr > best->addr))
            best = &symbols[i];
    }
    if (best == NULL)
        return NULL;
    *offset = addr - best->addr;
    return best->name;
}

void
print_location(uint32_t addr)
{
    uint32_t offset;
    const char *name = symbol_for(addr, &offset);
    fprintf(stderr, "0x%08x", addr);
    if (name != NULL)
        fprintf(stderr, " %s+%u", name, offset);
}

void
report_fault(Machine *m)
{
    char text[96] = "?";
    uint32_t ins;
    if (machine_fetch(m, m->fault_pc, &ins))
        disasm(ins, m->fault_pc, text, sizeof(text));

    fprintf(stderr, "fault: %s at ", fault_name(m->fault));
    print_location(m->fault_pc);
    fprintf(stderr, ": %s\n", text);
    if (m->fault == FAULT_DATA_ADDRESS || m->fault == FAULT_ALIGNMENT || m->fault == FAULT_STACK || m->fault == FAULT_PORT)
        fprintf(stderr, "  address 0x%08x\n", m->fault_addr);
    static const char *names[REG_COUNT] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "sp" };
    for (int i = 0; i < REG_COUNT; i++)
        fprintf(stderr, "  %s %08x%s", names[i], m->r[i], i % 3 == 2 ? "\n" : "");

    // Anything on the stack that looks like a code address is probably a
    // return address.
    fprintf(stderr, "  stack:\n");
    int shown = 0;
    for (uint32_t sp = m->r[REG_SP]; shown < 12; sp += 4) {
        const uint8_t *p = machine_data(m, sp, 4);
        if (p == NULL)
            break;
        uint32_t v = read32(p);
        if (v % 4 == 0 && v >= NULL_GUARD + 4 && v - NULL_GUARD <= m->space.code.size) {
            fprintf(stderr, "    ");
            print_location(v - 4);
            fprintf(stderr, "\n");
            shown++;
        }
    }
}
