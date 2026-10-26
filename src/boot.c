// The full machine: firmware loads the kernel from a disk image, and the host
// loop runs it with its devices. The console is the host terminal.

#define _POSIX_C_SOURCE 200809L

#include "host.h"

#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BLOCK_SIZE 4096

enum {
    PORT_CONSOLE_WRITE = 0x20,
    PORT_CONSOLE_READ = 0x21,
    PORT_CLOCK = 0x22,
    PORT_TIMER = 0x23,
    PORT_DISK_BLOCK = 0x30,
    PORT_DISK_ADDRESS = 0x31,
    PORT_DISK_COUNT = 0x32,
    PORT_DISK_COMMAND = 0x33,
    PORT_DISK_STATUS = 0x34,
    PORT_DISK_SIZE = 0x35,
};

enum {
    IRQ_TIMER = 1,
    IRQ_CONSOLE = 2,
};

enum {
    CONSOLE_EMPTY = 0xffffffffu,
    CONSOLE_CLOSED = 0xfffffffeu,
};

typedef struct {
    uint8_t input[4096];
    size_t input_start;
    size_t input_end;
    bool input_closed;

    uint64_t start;         // microseconds
    uint64_t timer;         // when the timer fires, or 0

    FILE *disk_file;
    uint8_t *disk;
    uint32_t disk_blocks;
    uint32_t disk_block;
    uint32_t disk_address;
    uint32_t disk_count;
    uint32_t disk_status;
} Host;

static uint64_t
now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}

// Copies count blocks between the disk and physical memory.
static uint32_t
disk_transfer(Machine *m, Host *h, bool write)
{
    uint64_t bytes = (uint64_t)h->disk_count * BLOCK_SIZE;
    if ((uint64_t)h->disk_block + h->disk_count > h->disk_blocks || h->disk_address + bytes > RAM_SIZE)
        return 1;
    uint8_t *disk = h->disk + (size_t)h->disk_block * BLOCK_SIZE;
    uint8_t *ram = m->ram + h->disk_address;
    if (!write) {
        memcpy(ram, disk, (size_t)bytes);
        return 0;
    }
    memcpy(disk, ram, (size_t)bytes);
    if (fseek(h->disk_file, (long)h->disk_block * BLOCK_SIZE, SEEK_SET) != 0 ||
        fwrite(disk, 1, (size_t)bytes, h->disk_file) != bytes || fflush(h->disk_file) != 0)
        return 1;
    return 0;
}

static bool
device_read(Machine *m, uint32_t port, uint32_t *value)
{
    Host *h = m->host;
    switch (port) {
    case PORT_CONSOLE_READ:
        if (h->input_start < h->input_end)
            *value = h->input[h->input_start++];
        else
            *value = h->input_closed ? CONSOLE_CLOSED : CONSOLE_EMPTY;
        return true;
    case PORT_CLOCK:
        *value = (uint32_t)(now() - h->start);
        return true;
    case PORT_DISK_STATUS:
        *value = h->disk_status;
        return true;
    case PORT_DISK_SIZE:
        *value = h->disk_blocks;
        return true;
    }
    return false;
}

static bool
device_write(Machine *m, uint32_t port, uint32_t value)
{
    Host *h = m->host;
    switch (port) {
    case PORT_CONSOLE_WRITE: {
        uint8_t c = (uint8_t)value;
        fwrite(&c, 1, 1, stdout);
        if (c == '\n')
            fflush(stdout);
        return true;
    }
    case PORT_TIMER:
        h->timer = value == 0 ? 0 : now() + value;
        return true;
    case PORT_DISK_BLOCK:
        h->disk_block = value;
        return true;
    case PORT_DISK_ADDRESS:
        h->disk_address = value;
        return true;
    case PORT_DISK_COUNT:
        h->disk_count = value;
        return true;
    case PORT_DISK_COMMAND:
        h->disk_status = (value == 1 || value == 2) ? disk_transfer(m, h, value == 2) : 1;
        return true;
    }
    return false;
}

// Moves waiting terminal input into the console buffer. With a timeout, waits
// up to that many microseconds for input first.
static void
read_console(Machine *m, Host *h, int timeout_ms)
{
    if (h->input_closed)
        return;
    if (h->input_start == h->input_end)
        h->input_start = h->input_end = 0;
    if (h->input_end == sizeof(h->input))
        return;
    struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
    if (poll(&p, 1, timeout_ms) <= 0)
        return;
    ssize_t n = read(STDIN_FILENO, h->input + h->input_end, sizeof(h->input) - h->input_end);
    if (n > 0)
        h->input_end += (size_t)n;
    else
        h->input_closed = true;
    m->pending |= IRQ_CONSOLE;
}

static void
check_timer(Machine *m, Host *h)
{
    if (h->timer != 0 && now() >= h->timer) {
        h->timer = 0;
        m->pending |= IRQ_TIMER;
    }
}

// Loads the kernel named by the disk's header. The kernel's data, heap and
// stack go at the start of RAM and its code right after; the kernel's data
// space covers all of RAM, so data address A is physical address A - 0x10000.
static bool
firmware(Machine *m, Host *h)
{
    if (h->disk_blocks == 0 || memcmp(h->disk, "TFS1", 4) != 0) {
        fprintf(stderr, "terrarium: not a Terrarium disk\n");
        return false;
    }
    uint32_t block = read32(h->disk + 28);
    uint32_t size = read32(h->disk + 32);
    if (block >= h->disk_blocks || size > (uint64_t)(h->disk_blocks - block) * BLOCK_SIZE) {
        fprintf(stderr, "terrarium: the disk's kernel is out of range\n");
        return false;
    }
    Executable exe;
    const char *error = parse_executable(h->disk + (size_t)block * BLOCK_SIZE, size, &exe);
    if (error != NULL) {
        fprintf(stderr, "terrarium: kernel: %s\n", error);
        return false;
    }
    uint32_t own = exe.memory - exe.code;
    memcpy(m->ram, exe.data_bytes, exe.data);
    memcpy(m->ram + own, exe.code_bytes, exe.code);

    m->space.code = (Segment){ .base = own, .size = exe.code };
    m->space.data = (Segment){ .base = 0, .size = RAM_SIZE };
    m->space.stack_limit = NULL_GUARD + own - exe.stack;
    m->pc = exe.entry;
    m->r[REG_SP] = NULL_GUARD + own;
    m->r[0] = NULL_GUARD + align4(exe.data) + align4(exe.bss);
    m->r[1] = m->space.stack_limit;
    m->r[2] = (own + exe.code + BLOCK_SIZE - 1) / BLOCK_SIZE * BLOCK_SIZE;
    m->r[3] = RAM_SIZE;
    m->kernel = true;
    m->has_kernel = true;
    return true;
}

int
boot(const char *disk_path, const char *symbols)
{
    Host h = { .start = now() };
    h.disk_file = fopen(disk_path, "r+b");
    size_t size = 0;
    h.disk = read_file(disk_path, &size);
    if (h.disk_file == NULL || h.disk == NULL) {
        fprintf(stderr, "terrarium: can't open %s\n", disk_path);
        return 2;
    }
    h.disk_blocks = (uint32_t)(size / BLOCK_SIZE);

    Machine m;
    if (!machine_init(&m)) {
        fprintf(stderr, "terrarium: out of memory\n");
        return 2;
    }
    m.host = &h;
    m.port_read = device_read;
    m.port_write = device_write;
    if (!firmware(&m, &h))
        return 2;
    if (symbols != NULL)
        load_symbols(symbols);

    int status = 0;
    for (;;) {
        read_console(&m, &h, 0);
        check_timer(&m, &h);
        Stop stop = machine_run(&m, 100000);
        if (stop == STOP_WAIT) {
            if (h.input_closed && h.timer == 0) {
                fprintf(stderr, "terrarium: idle with no input left, stopping\n");
                break;
            }
            int timeout = -1;
            if (h.timer != 0) {
                uint64_t t = now();
                timeout = h.timer > t ? (int)((h.timer - t + 999) / 1000) : 0;
            }
            if (h.input_closed) {
                struct timespec t = { .tv_sec = timeout / 1000, .tv_nsec = (long)(timeout % 1000) * 1000000 };
                nanosleep(&t, NULL);
            } else {
                read_console(&m, &h, timeout);
            }
        } else if (stop == STOP_HALT) {
            break;
        } else if (stop == STOP_FAULT) {
            fprintf(stderr, "kernel ");
            report_fault(&m);
            status = 70;
            break;
        }
    }
    fflush(stdout);
    machine_free(&m);
    return status;
}
