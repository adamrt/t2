// The development monitor runs one executable directly on the machine and
// answers its system calls using the host's files.

#include "machine.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    SYS_EXIT,
    SYS_READ,
    SYS_WRITE,
    SYS_OPEN,
    SYS_CLOSE,
    SYS_ARGS,
    SYS_ARG,
};

#define HEADER_SIZE 28
#define FAILED      0xffffffffu

typedef struct {
    uint32_t addr;
    char name[64];
} Symbol;

static Symbol *symbols;
static size_t symbol_count;

static uint32_t
read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t
align4(uint32_t n)
{
    return (n + 3) & ~3u;
}

static uint8_t *
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

// Symbols come from an optional "name.sym" file next to "name.bin", with
// lines of the form "c 00010000 start". Only code symbols are used.
static void
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

static void
print_location(uint32_t addr)
{
    uint32_t offset;
    const char *name = symbol_for(addr, &offset);
    fprintf(stderr, "0x%08x", addr);
    if (name != NULL)
        fprintf(stderr, " %s+%u", name, offset);
}

static bool
load_program(Machine *m, const char *path)
{
    size_t size;
    uint8_t *file = read_file(path, &size);
    if (file == NULL) {
        fprintf(stderr, "terrarium: can't read %s\n", path);
        return false;
    }

    const char *error = NULL;
    uint32_t entry = 0, code = 0, data = 0, bss = 0, stack = 0, memory = 0;
    if (size < HEADER_SIZE || memcmp(file, "TRX1", 4) != 0) {
        error = "not an executable";
    } else {
        entry = read32(file + 4);
        code = read32(file + 8);
        data = read32(file + 12);
        bss = read32(file + 16);
        stack = read32(file + 20);
        memory = read32(file + 24);
        if (code % 4 != 0 || memory % 4 != 0 || (uint64_t)HEADER_SIZE + code + data != size)
            error = "malformed header";
        else if (memory > RAM_SIZE || (uint64_t)code + align4(data) + align4(bss) + stack > memory)
            error = "doesn't fit in its declared memory";
        else if (entry < NULL_GUARD || entry - NULL_GUARD >= code || entry % 4 != 0)
            error = "bad entry point";
    }
    if (error != NULL) {
        fprintf(stderr, "terrarium: %s: %s\n", path, error);
        free(file);
        return false;
    }

    uint32_t data_space = memory - code;
    memcpy(m->ram, file + HEADER_SIZE, code);
    memcpy(m->ram + code, file + HEADER_SIZE + code, data);
    free(file);

    m->space.code = (Segment){ .base = 0, .size = code };
    m->space.data = (Segment){ .base = code, .size = data_space };
    m->space.stack_limit = NULL_GUARD + data_space - stack;
    m->pc = entry;
    m->r[REG_SP] = NULL_GUARD + data_space;
    m->r[0] = NULL_GUARD + align4(data) + align4(bss);
    m->r[1] = m->space.stack_limit;
    return true;
}

static uint32_t
open_file(Machine *m, uint32_t addr, uint32_t len, uint32_t mode)
{
    char path[1024];
    const uint8_t *p = machine_data(m, addr, len);
    if (p == NULL || len >= sizeof(path) || mode > 1)
        return FAILED;
    memcpy(path, p, len);
    path[len] = '\0';
    int fd = mode == 0 ? open(path, O_RDONLY) : open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    return fd < 0 ? FAILED : (uint32_t)fd;
}

// Returns false when the program exits.
static bool
system_call(Machine *m, int argc, char **argv, int *status)
{
    uint32_t *r = m->r;
    uint8_t *p;

    switch (m->sys) {
    case SYS_EXIT:
        *status = (int)(r[0] & 0xff);
        return false;
    case SYS_READ:
        p = machine_data(m, r[1], r[2]);
        r[0] = p == NULL ? FAILED : (uint32_t)read((int)r[0], p, r[2]);
        break;
    case SYS_WRITE:
        p = machine_data(m, r[1], r[2]);
        r[0] = p == NULL ? FAILED : (uint32_t)write((int)r[0], p, r[2]);
        break;
    case SYS_OPEN:
        r[0] = open_file(m, r[0], r[1], r[2]);
        break;
    case SYS_CLOSE:
        r[0] = r[0] <= 2 ? FAILED : (uint32_t)close((int)r[0]);
        break;
    case SYS_ARGS:
        r[0] = (uint32_t)argc;
        break;
    case SYS_ARG:
        if (r[0] >= (uint32_t)argc) {
            r[0] = FAILED;
            break;
        }
        uint32_t len = (uint32_t)strlen(argv[r[0]]);
        uint32_t n = len < r[2] ? len : r[2];
        p = machine_data(m, r[1], n);
        if (p == NULL) {
            r[0] = FAILED;
            break;
        }
        memcpy(p, argv[r[0]], n);
        r[0] = len;
        break;
    default:
        fprintf(stderr, "terrarium: unknown system call %u at ", m->sys);
        print_location(m->pc - 4);
        fprintf(stderr, "\n");
        r[0] = FAILED;
        break;
    }
    return true;
}

static void
report_fault(Machine *m)
{
    char text[96] = "?";
    uint32_t ins;
    if (machine_fetch(m, m->fault_pc, &ins))
        disasm(ins, m->fault_pc, text, sizeof(text));

    fprintf(stderr, "fault: %s at ", fault_name(m->fault));
    print_location(m->fault_pc);
    fprintf(stderr, ": %s\n", text);
    if (m->fault == FAULT_DATA_ADDRESS || m->fault == FAULT_ALIGNMENT || m->fault == FAULT_STACK)
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

static void
usage(void)
{
    fprintf(stderr, "usage: terrarium [--trace] [--stats] program.bin [arguments...]\n");
    exit(2);
}

int
main(int argc, char **argv)
{
    bool trace = false;
    bool stats = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
        if (strcmp(argv[i], "--trace") == 0)
            trace = true;
        else if (strcmp(argv[i], "--stats") == 0)
            stats = true;
        else
            usage();
    }
    if (i >= argc)
        usage();

    Machine m;
    if (!machine_init(&m)) {
        fprintf(stderr, "terrarium: out of memory\n");
        return 2;
    }
    const char *program = argv[i];
    if (!load_program(&m, program))
        return 2;
    load_symbols(program);

    int prog_argc = argc - i - 1;
    char **prog_argv = argv + i + 1;
    int status = 0;

    for (;;) {
        if (trace) {
            uint32_t ins;
            char text[96] = "?";
            if (machine_fetch(&m, m.pc, &ins))
                disasm(ins, m.pc, text, sizeof(text));
            print_location(m.pc);
            fprintf(stderr, "  %s\n", text);
        }

        Stop stop = machine_run(&m, trace ? 1 : 1000000);
        if (stop == STOP_SYS) {
            if (!system_call(&m, prog_argc, prog_argv, &status))
                break;
        } else if (stop == STOP_HALT) {
            status = (int)(m.r[0] & 0xff);
            break;
        } else if (stop == STOP_FAULT) {
            report_fault(&m);
            status = 70;
            break;
        }
    }

    if (stats)
        fprintf(stderr, "cycles: %llu\n", (unsigned long long)m.cycles);
    machine_free(&m);
    return status;
}
