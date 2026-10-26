// The full machine: firmware loads the kernel from a disk image, and the host
// loop runs it with its devices. The console is the host terminal.

#define _POSIX_C_SOURCE 200809L

#include "audio.h"
#include "gui.h"
#include "host.h"
#include "net.h"

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
    PORT_INPUT_COUNT = 0x58,
    PORT_INPUT_READ = 0x59,
    PORT_AUDIO_RATE = 0x60,
    PORT_AUDIO_ADDRESS = 0x61,
    PORT_AUDIO_LENGTH = 0x62,
    PORT_AUDIO_PLAY = 0x63,
    PORT_AUDIO_QUEUED = 0x64,
    PORT_AUDIO_STATUS = 0x65,
};

enum {
    IRQ_TIMER = 1,
    IRQ_CONSOLE = 2,
    IRQ_INPUT = 4,
    IRQ_VBLANK = 8,
    IRQ_NET = 16,
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

    Gui *gui;
    Gpu gpu;
    InputQueue events;
    bool vblank;            // the kernel wants vertical blank interrupts
    uint64_t next_vblank;

    Net net;
    Audio *audio;
    uint32_t audio_address;
    uint32_t audio_length;
    uint32_t audio_status;
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
    case PORT_INPUT_COUNT:
        *value = h->events.count;
        return true;
    case PORT_AUDIO_RATE:
        *value = AUDIO_RATE;
        return true;
    case PORT_AUDIO_QUEUED:
        *value = audio_queued(h->audio);
        return true;
    case PORT_AUDIO_STATUS:
        *value = h->audio_status;
        return true;
    }
    if (net_read(&h->net, port, value))
        return true;
    return gpu_read(&h->gpu, port, value);
}

// Copies the oldest input event to a physical address.
static bool
read_input(Machine *m, Host *h, uint32_t addr)
{
    if (addr > RAM_SIZE - 16)
        return false;
    InputEvent e = { 0 };
    if (h->events.count > 0) {
        e = h->events.events[h->events.start];
        h->events.start = (h->events.start + 1) % INPUT_QUEUE;
        h->events.count--;
    }
    uint32_t words[4] = { e.type, e.a, e.b, e.c };
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++)
            m->ram[addr + (uint32_t)(i * 4 + j)] = (uint8_t)(words[i] >> (j * 8));
    }
    return true;
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
    case PORT_VIDEO_VBLANK:
        h->vblank = value != 0;
        return true;
    case PORT_INPUT_READ:
        return read_input(m, h, value);
    case PORT_AUDIO_ADDRESS:
        h->audio_address = value;
        return true;
    case PORT_AUDIO_LENGTH:
        h->audio_length = value;
        return true;
    case PORT_AUDIO_PLAY:
        if (h->audio_address > RAM_SIZE || h->audio_length > RAM_SIZE - h->audio_address || h->audio_length % 2 != 0)
            h->audio_status = 1;
        else
            h->audio_status = audio_queue(h->audio, m->ram + h->audio_address, h->audio_length) ? 0 : 1;
        return true;
    }
    if (net_write(&h->net, m, port, value))
        return true;
    return gpu_write(&h->gpu, m, port, value);
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

// Raises the interrupts for waiting input and the vertical blank.
static void
check_video(Machine *m, Host *h)
{
    if (h->events.count > 0)
        m->pending |= IRQ_INPUT;
    uint64_t t = now();
    if (h->vblank && t >= h->next_vblank) {
        m->pending |= IRQ_VBLANK;
        h->next_vblank = t + 16667;
    }
}

// How long the machine can sleep, in milliseconds, or -1 for as long as it likes.
static int
idle_time(Host *h)
{
    uint64_t t = now();
    uint64_t until = 0;
    if (h->timer != 0)
        until = h->timer;
    if (h->vblank && (until == 0 || h->next_vblank < until))
        until = h->next_vblank;
    if (until == 0)
        return h->gui != NULL ? 50 : -1;
    int ms = until > t ? (int)((until - t + 999) / 1000) : 0;
    return h->gui != NULL && ms > 50 ? 50 : ms;
}

static void
put32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    fwrite(b, 1, 4, f);
}

static uint32_t
crc(uint32_t c, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return c;
}

static void
chunk(FILE *f, const char *type, const uint8_t *data, size_t n)
{
    put32(f, (uint32_t)n);
    uint32_t c = crc(0xffffffffu, (const uint8_t *)type, 4);
    c = crc(c, data, n);
    fwrite(type, 1, 4, f);
    fwrite(data, 1, n, f);
    put32(f, ~c);
}

// Saves the front buffer as an uncompressed PNG, for looking at from the host.
static void
screenshot(Gpu *g, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL || g->width == 0)
        return;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t header[13] = { 0 };
    for (int i = 0; i < 4; i++) {
        header[i] = (uint8_t)(g->width >> (24 - 8 * i));
        header[4 + i] = (uint8_t)(g->height >> (24 - 8 * i));
    }
    header[8] = 8;
    header[9] = 2;
    chunk(f, "IHDR", header, 13);

    size_t row = (size_t)g->width * 3 + 1;
    size_t raw_size = row * g->height;
    uint8_t *raw = malloc(raw_size);
    for (uint32_t y = 0; y < g->height; y++) {
        uint8_t *r = raw + y * row;
        r[0] = 0;
        for (uint32_t x = 0; x < g->width; x++) {
            uint32_t p = g->front[(size_t)y * g->width + x];
            r[1 + x * 3] = (uint8_t)(p >> 16);
            r[2 + x * 3] = (uint8_t)(p >> 8);
            r[3 + x * 3] = (uint8_t)p;
        }
    }
    // zlib with stored blocks of at most 65535 bytes.
    size_t blocks = (raw_size + 65534) / 65535;
    size_t zsize = 2 + raw_size + blocks * 5 + 4;
    uint8_t *z = malloc(zsize);
    size_t at = 0;
    z[at++] = 0x78;
    z[at++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_size; i += 65535) {
        size_t n = raw_size - i < 65535 ? raw_size - i : 65535;
        z[at++] = i + n == raw_size ? 1 : 0;
        z[at++] = (uint8_t)n;
        z[at++] = (uint8_t)(n >> 8);
        z[at++] = (uint8_t)~n;
        z[at++] = (uint8_t)(~n >> 8);
        memcpy(z + at, raw + i, n);
        at += n;
        for (size_t k = 0; k < n; k++) {
            a = (a + raw[i + k]) % 65521;
            b = (b + a) % 65521;
        }
    }
    uint32_t adler = b << 16 | a;
    for (int i = 0; i < 4; i++)
        z[at++] = (uint8_t)(adler >> (24 - 8 * i));
    chunk(f, "IDAT", z, at);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

int
boot(const BootOptions *o)
{
    const char *disk_path = o->disk;
    Host h = { .start = now() };
    Machine m;
    if (!machine_init(&m)) {
        fprintf(stderr, "terrarium: out of memory\n");
        return 2;
    }
    uint32_t width = o->width, height = o->height;
    if (o->window) {
        h.gui = gui_open(o->fullscreen, &width, &height);
        if (h.gui == NULL)
            return 2;
    }
    h.audio = audio_open(!o->window || o->mute);
    net_init(&h.net);
    if (!gpu_init(&h.gpu, width, height)) {
        fprintf(stderr, "terrarium: out of memory\n");
        return 2;
    }
    h.disk_file = fopen(disk_path, "r+b");
    size_t size = 0;
    h.disk = read_file(disk_path, &size);
    if (h.disk_file == NULL || h.disk == NULL) {
        fprintf(stderr, "terrarium: can't open %s\n", disk_path);
        return 2;
    }
    h.disk_blocks = (uint32_t)(size / BLOCK_SIZE);

    m.host = &h;
    m.port_read = device_read;
    m.port_write = device_write;
    if (!firmware(&m, &h))
        return 2;
    if (o->symbols != NULL)
        load_symbols(o->symbols);

    int status = 0;
    const char *typing = o->type;
    uint64_t next_key = h.start + 500000;
    for (;;) {
        if (o->run_for != 0 && now() - h.start > (uint64_t)o->run_for * 1000)
            break;
        // Typed keys arrive one at a time, as if from a quick typist.
        if (typing != NULL && *typing != '\0' && now() >= next_key && h.events.count < INPUT_QUEUE) {
            uint32_t c = (uint8_t)*typing++;
            InputEvent e = c == '\n' ? (InputEvent){ INPUT_KEY_DOWN, 10, 0, 0 } : (InputEvent){ INPUT_TEXT, c, 0, 0 };
            h.events.events[(h.events.start + h.events.count) % INPUT_QUEUE] = e;
            h.events.count++;
            next_key = now() + 20000;
        }
        read_console(&m, &h, 0);
        check_timer(&m, &h);
        if (h.gui != NULL && !gui_poll(h.gui, &h.events))
            break;
        check_video(&m, &h);
        if (net_poll(&h.net))
            m.pending |= IRQ_NET;
        Stop stop = machine_run(&m, 100000);
        if (h.gui != NULL)
            gui_show(h.gui, &h.gpu);
        if (stop == STOP_WAIT) {
            int timeout = idle_time(&h);
            if (net_active(&h.net) && (timeout < 0 || timeout > 10))
                timeout = 10;
            if (h.input_closed && timeout < 0) {
                fprintf(stderr, "terrarium: idle with no input left, stopping\n");
                break;
            }
            if (h.gui != NULL) {
                gui_wait(h.gui, h.input_closed ? timeout : (timeout > 10 ? 10 : timeout));
            } else if (h.input_closed) {
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
    if (o->screenshot != NULL)
        screenshot(&h.gpu, o->screenshot);
    audio_close(h.audio);
    gui_close(h.gui);
    machine_free(&m);
    return status;
}
