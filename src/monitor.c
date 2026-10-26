// The development monitor runs one executable directly on the machine and
// answers its system calls using the host's files.

#define _POSIX_C_SOURCE 200809L

#include "host.h"

#include <fcntl.h>
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

static bool
load_program(Machine *m, const char *path)
{
    size_t size;
    uint8_t *file = read_file(path, &size);
    if (file == NULL) {
        fprintf(stderr, "terrarium: can't read %s\n", path);
        return false;
    }
    Executable exe;
    const char *error = parse_executable(file, size, &exe);
    if (error == NULL && (uint64_t)exe.header_size + exe.code + exe.data != size)
        error = "malformed header";
    if (error != NULL) {
        fprintf(stderr, "terrarium: %s: %s\n", path, error);
        free(file);
        return false;
    }

    uint32_t data_space = exe.memory - exe.code;
    memcpy(m->ram, exe.code_bytes, exe.code);
    memcpy(m->ram + exe.code, exe.data_bytes, exe.data);
    free(file);

    m->space.code = (Segment){ .base = 0, .size = exe.code };
    m->space.data = (Segment){ .base = exe.code, .size = data_space };
    m->space.stack_limit = NULL_GUARD + data_space - exe.stack;
    m->pc = exe.entry;
    m->r[REG_SP] = NULL_GUARD + data_space;
    m->r[0] = NULL_GUARD + align4(exe.data) + align4(exe.bss);
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
usage(void)
{
    fprintf(stderr, "usage: terrarium [--trace] [--stats] program.bin [arguments...]\n"
                    "       terrarium boot [--headless] [--fullscreen] [--mute] disk.img [kernel.sym]\n"
                    "       terrarium boot [--screen WxH] [--run-for ms] [--screenshot out.png] disk.img\n");
    exit(2);
}

int
main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "boot") == 0) {
        BootOptions o = { .window = true };
        int i = 2;
        for (; i < argc && argv[i][0] == '-'; i++) {
            if (strcmp(argv[i], "--headless") == 0) {
                o.window = false;
            } else if (strcmp(argv[i], "--fullscreen") == 0) {
                o.fullscreen = true;
            } else if (strcmp(argv[i], "--mute") == 0) {
                o.mute = true;
            } else if (strcmp(argv[i], "--screen") == 0 && i + 1 < argc) {
                if (sscanf(argv[++i], "%ux%u", &o.width, &o.height) != 2)
                    usage();
                o.window = false;
            } else if (strcmp(argv[i], "--run-for") == 0 && i + 1 < argc) {
                o.run_for = (uint32_t)atoi(argv[++i]);
            } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
                o.screenshot = argv[++i];
            } else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
                o.type = argv[++i];
            } else {
                usage();
            }
        }
        if (i == argc || argc - i > 2)
            usage();
        o.disk = argv[i];
        o.symbols = i + 1 < argc ? argv[i + 1] : NULL;
        return boot(&o);
    }

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
        } else if (stop == STOP_FAULT) {
            report_fault(&m);
            status = 70;
            break;
        } else if (stop != STOP_BUDGET) {
            fprintf(stderr, "terrarium: unexpected stop %d\n", stop);
            status = 70;
            break;
        }
    }

    if (stats)
        fprintf(stderr, "cycles: %llu\n", (unsigned long long)m.cycles);
    machine_free(&m);
    return status;
}
