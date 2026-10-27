// The development monitor runs one executable directly on the machine and
// answers its system calls using the host's files.

#define _POSIX_C_SOURCE 200809L

#include "host.h"
#include "boot.h"

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
load_program(Machine *machine, const char *path)
{
    size_t size;
    uint8_t *file = read_file(path, &size);
    if (file == NULL) {
        fprintf(stderr, "terrarium: can't read %s\n", path);
        return false;
    }
    Executable exe;
    const char *error = parse_executable(file, size, &exe);
    if (error == NULL && (uint64_t)EXECUTABLE_HEADER_SIZE + exe.code_size + exe.data_size != size)
        error = "malformed header";
    if (error != NULL) {
        fprintf(stderr, "terrarium: %s: %s\n", path, error);
        free(file);
        return false;
    }

    uint32_t data_size = exe.memory_size - exe.code_size;
    memcpy(machine->ram, exe.code_bytes, exe.code_size);
    memcpy(machine->ram + exe.code_size, exe.data_bytes, exe.data_size);
    free(file);

    machine->space.code = (Segment){ .base = 0, .size = exe.code_size };
    machine->space.data = (Segment){ .base = exe.code_size, .size = data_size };
    machine->space.stack_limit = NULL_GUARD + data_size - exe.stack_size;
    machine->pc = exe.entry_pc;
    machine->r[REG_SP] = NULL_GUARD + data_size;
    machine->r[0] = NULL_GUARD + align4(exe.data_size) + align4(exe.bss_size);
    machine->r[1] = machine->space.stack_limit;
    return true;
}

static uint32_t
kib(uint32_t bytes)
{
    return (bytes + 1023) / 1024;
}

// Memory starts zeroed, so how far the program wrote into its heap and its
// stack shows how much of each it used. The kernel's trace reports the same.
static void
print_memory_used(Machine *machine, uint32_t heap_start)
{
    const uint8_t *data = machine->ram + machine->space.data.base;
    uint32_t start = heap_start - NULL_GUARD;
    uint32_t limit = machine->space.stack_limit - NULL_GUARD;
    uint32_t top = machine->space.data.size;
    uint32_t end = limit;
    while (end > start && data[end - 1] == 0)
        end--;
    uint32_t low = limit;
    while (low < top && data[low] == 0)
        low++;
    fprintf(stderr, "memory: used %u KiB of %u KiB heap, %u KiB of %u KiB stack\n",
            kib(end - start), kib(limit - start), kib(top - low), kib(top - limit));
}

static uint32_t
open_file(Machine *machine, uint32_t addr, uint32_t len, uint32_t mode)
{
    char path[1024];
    const uint8_t *p = machine_data(machine, addr, len);
    if (p == NULL || len >= sizeof(path) || mode > 1)
        return HOST_FAILED;
    memcpy(path, p, len);
    path[len] = '\0';
    int fd = mode == 0 ? open(path, O_RDONLY) : open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    return fd < 0 ? HOST_FAILED : (uint32_t)fd;
}

// Returns false when the program exits.
static bool
handle_system_call(Machine *machine, const SymbolTable *symbols, int argc, char **argv, int *status)
{
    uint32_t *r = machine->r;
    uint8_t *p;

    switch (machine->sys) {
    case SYS_EXIT:
        *status = (int)(r[0] & 0xff);
        return false;
    case SYS_READ:
        p = machine_data(machine, r[1], r[2]);
        r[0] = p == NULL ? HOST_FAILED : (uint32_t)read((int)r[0], p, r[2]);
        break;
    case SYS_WRITE:
        p = machine_data(machine, r[1], r[2]);
        r[0] = p == NULL ? HOST_FAILED : (uint32_t)write((int)r[0], p, r[2]);
        break;
    case SYS_OPEN:
        r[0] = open_file(machine, r[0], r[1], r[2]);
        break;
    case SYS_CLOSE:
        r[0] = r[0] <= 2 ? HOST_FAILED : (uint32_t)close((int)r[0]);
        break;
    case SYS_ARGS:
        r[0] = (uint32_t)argc;
        break;
    case SYS_ARG:
        if (r[0] >= (uint32_t)argc) {
            r[0] = HOST_FAILED;
            break;
        }
        uint32_t len = (uint32_t)strlen(argv[r[0]]);
        uint32_t n = len < r[2] ? len : r[2];
        p = machine_data(machine, r[1], n);
        if (p == NULL) {
            r[0] = HOST_FAILED;
            break;
        }
        memcpy(p, argv[r[0]], n);
        r[0] = len;
        break;
    default:
        fprintf(stderr, "terrarium: unknown system call %u at ", machine->sys);
        print_location(symbols, machine->pc - 4);
        fprintf(stderr, "\n");
        r[0] = HOST_FAILED;
        break;
    }
    return true;
}

static void
usage(void)
{
    fprintf(
        stderr,
        "usage: terrarium [--trace] [--stats] program.bin [arguments...]\n"
        "       terrarium boot [--headless] [--fullscreen] [--mute] disk.img [kernel.sym]\n"
        "       terrarium boot [--screen WxH] [--run-for ms] [--screenshot out.png] disk.img\n");
    exit(2);
}

static BootOptions
parse_boot_options(int argc, char **argv)
{
    BootOptions options = { .window = true };
    int i = 2;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            options.window = false;
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            options.fullscreen = true;
        } else if (strcmp(argv[i], "--mute") == 0) {
            options.mute = true;
        } else if (strcmp(argv[i], "--screen") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%ux%u", &options.screen_width, &options.screen_height) != 2)
                usage();
            options.window = false;
        } else if (strcmp(argv[i], "--run-for") == 0 && i + 1 < argc) {
            options.run_for_ms = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            options.screenshot_path = argv[++i];
        } else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
            options.typed_text = argv[++i];
        } else if (strcmp(argv[i], "--date") == 0 && i + 1 < argc) {
            options.fixed_date = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--drag") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%d,%d,%d,%d", &options.drag[0], &options.drag[1],
                       &options.drag[2], &options.drag[3]) != 4)
                usage();
            options.has_drag = true;
        } else {
            usage();
        }
    }
    if (i == argc || argc - i > 2)
        usage();
    options.disk_path = argv[i];
    options.symbol_path = i + 1 < argc ? argv[i + 1] : NULL;
    return options;
}

int
main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "boot") == 0) {
        BootOptions options = parse_boot_options(argc, argv);
        return boot(&options);
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

    Machine machine;
    if (!machine_init(&machine)) {
        fprintf(stderr, "terrarium: out of memory\n");
        return 2;
    }
    const char *program = argv[i];
    if (!load_program(&machine, program)) {
        machine_free(&machine);
        return 2;
    }
    SymbolTable symbols = { 0 };
    symbols_load(&symbols, program);
    uint32_t heap_start = machine.r[0];

    int program_argc = argc - i - 1;
    char **program_argv = argv + i + 1;
    int status = 0;

    for (;;) {
        if (trace) {
            uint32_t instruction;
            char text[96] = "?";
            if (machine_fetch(&machine, machine.pc, &instruction))
                disasm(instruction, machine.pc, text, sizeof(text));
            print_location(&symbols, machine.pc);
            fprintf(stderr, "  %s\n", text);
        }

        Stop stop = machine_run(&machine, trace ? 1 : 1000000);
        if (stop == STOP_SYS) {
            if (!handle_system_call(&machine, &symbols, program_argc, program_argv, &status))
                break;
        } else if (stop == STOP_FAULT) {
            report_fault(&machine, &symbols);
            status = 70;
            break;
        } else if (stop != STOP_BUDGET) {
            fprintf(stderr, "terrarium: unexpected stop %d\n", stop);
            status = 70;
            break;
        }
    }

    if (stats) {
        fprintf(stderr, "cycles: %llu\n", (unsigned long long)machine.cycles);
        print_memory_used(&machine, heap_start);
    }
    symbols_free(&symbols);
    machine_free(&machine);
    return status;
}
