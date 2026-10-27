// Device registers and host I/O for a booted machine.
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE // tm_gmtoff on glibc.

#include "devices.h"
#include "bytes.h"
#include "host.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define VBLANK_INTERVAL_US 16667
#define NETWORK_POLL_MS 10

enum {
    PORT_CONSOLE_WRITE = 0x20,
    PORT_CONSOLE_READ = 0x21,
    PORT_CLOCK = 0x22,
    PORT_TIMER = 0x23,
    PORT_DATE = 0x24,
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

#define CONSOLE_EMPTY 0xffffffffu
#define CONSOLE_CLOSED 0xfffffffeu

// Copies count blocks between the disk and physical memory.
static uint32_t
disk_transfer(Machine *machine, Devices *devices, bool write)
{
    uint64_t bytes = (uint64_t)devices->disk.count * DISK_BLOCK_SIZE;
    if ((uint64_t)devices->disk.block + devices->disk.count > devices->disk.blocks ||
        devices->disk.address + bytes > RAM_SIZE)
        return 1;
    uint8_t *disk = devices->disk.bytes + (size_t)devices->disk.block * DISK_BLOCK_SIZE;
    uint8_t *ram = machine->ram + devices->disk.address;
    if (!write) {
        memcpy(ram, disk, (size_t)bytes);
        return 0;
    }
    memcpy(disk, ram, (size_t)bytes);
    if (fseek(devices->disk.file, (long)devices->disk.block * DISK_BLOCK_SIZE, SEEK_SET) != 0 ||
        fwrite(disk, 1, (size_t)bytes, devices->disk.file) != bytes ||
        fflush(devices->disk.file) != 0)
        return 1;
    return 0;
}

static bool
devices_read_port(Machine *machine, uint32_t port, uint32_t *value)
{
    Devices *devices = machine->host;
    switch (port) {
    case PORT_CONSOLE_READ:
        if (devices->console.start < devices->console.end)
            *value = devices->console.bytes[devices->console.start++];
        else
            *value = devices->console.closed ? CONSOLE_CLOSED : CONSOLE_EMPTY;
        return true;
    case PORT_CLOCK:
        *value = (uint32_t)(host_monotonic_us() - devices->started_at_us);
        return true;
    case PORT_DATE: {
        if (devices->fixed_date != 0) {
            *value = devices->fixed_date;
            return true;
        }
        // Local time, counted in seconds as if the local zone were UTC.
        time_t t = time(NULL);
        struct tm local;
        localtime_r(&t, &local);
        *value = (uint32_t)(t + local.tm_gmtoff);
        return true;
    }
    case PORT_DISK_STATUS:
        *value = devices->disk.status;
        return true;
    case PORT_DISK_SIZE:
        *value = devices->disk.blocks;
        return true;
    case PORT_INPUT_COUNT:
        *value = devices->input.count;
        return true;
    case PORT_AUDIO_RATE:
        *value = AUDIO_RATE;
        return true;
    case PORT_AUDIO_QUEUED:
        *value = audio_queued(devices->audio.output);
        return true;
    case PORT_AUDIO_STATUS:
        *value = devices->audio.status;
        return true;
    }
    if (net_read(&devices->net, port, value))
        return true;
    return gpu_read(&devices->gpu, port, value);
}

// Copies the oldest input event to a physical address.
static bool
input_read(Machine *machine, Devices *devices, uint32_t addr)
{
    if (addr > RAM_SIZE - 16)
        return false;
    InputEvent event = input_pop(&devices->input);
    uint32_t words[4] = { event.type, event.a, event.b, event.c };
    for (uint32_t i = 0; i < 4; i++)
        write_u32_le(machine->ram + addr + i * 4, words[i]);
    return true;
}

static bool
devices_write_port(Machine *machine, uint32_t port, uint32_t value)
{
    Devices *devices = machine->host;
    switch (port) {
    case PORT_CONSOLE_WRITE: {
        uint8_t c = (uint8_t)value;
        fwrite(&c, 1, 1, stdout);
        if (c == '\n')
            fflush(stdout);
        return true;
    }
    case PORT_TIMER:
        devices->timer_at_us = value == 0 ? 0 : host_monotonic_us() + value;
        return true;
    case PORT_DISK_BLOCK:
        devices->disk.block = value;
        return true;
    case PORT_DISK_ADDRESS:
        devices->disk.address = value;
        return true;
    case PORT_DISK_COUNT:
        devices->disk.count = value;
        return true;
    case PORT_DISK_COMMAND:
        devices->disk.status =
            (value == 1 || value == 2) ? disk_transfer(machine, devices, value == 2) : 1;
        return true;
    case PORT_VIDEO_VBLANK:
        devices->vblank_enabled = value != 0;
        return true;
    case PORT_INPUT_READ:
        return input_read(machine, devices, value);
    case PORT_AUDIO_ADDRESS:
        devices->audio.address = value;
        return true;
    case PORT_AUDIO_LENGTH:
        devices->audio.length = value;
        return true;
    case PORT_AUDIO_PLAY:
        if (devices->audio.address > RAM_SIZE ||
            devices->audio.length > RAM_SIZE - devices->audio.address ||
            devices->audio.length % 2 != 0)
            devices->audio.status = 1;
        else
            devices->audio.status =
                audio_queue(devices->audio.output, machine->ram + devices->audio.address,
                            devices->audio.length)
                    ? 0
                    : 1;
        return true;
    }
    if (net_write(&devices->net, machine, port, value))
        return true;
    return gpu_write(&devices->gpu, machine, port, value);
}

// Moves waiting terminal input into the console buffer. With a timeout, waits
// up to that many milliseconds for input first.
static void
console_poll(Machine *machine, Devices *devices, int timeout_ms)
{
    if (devices->console.closed)
        return;
    if (devices->console.start == devices->console.end)
        devices->console.start = devices->console.end = 0;
    if (devices->console.end == sizeof(devices->console.bytes))
        return;
    struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
    if (poll(&p, 1, timeout_ms) <= 0)
        return;
    ssize_t n = read(STDIN_FILENO, devices->console.bytes + devices->console.end,
                     sizeof(devices->console.bytes) - devices->console.end);
    if (n > 0)
        devices->console.end += (size_t)n;
    else if (n == 0 || (errno != EINTR && errno != EAGAIN))
        devices->console.closed = true;
    else
        return;
    machine->pending |= IRQ_CONSOLE;
}

static void
timer_poll(Machine *machine, Devices *devices)
{
    if (devices->timer_at_us != 0 && host_monotonic_us() >= devices->timer_at_us) {
        devices->timer_at_us = 0;
        machine->pending |= IRQ_TIMER;
    }
}

// Raises the interrupts for waiting input and the vertical blank.
static void
video_poll(Machine *machine, Devices *devices)
{
    if (devices->input.count > 0)
        machine->pending |= IRQ_INPUT;
    uint64_t t = host_monotonic_us();
    if (devices->vblank_enabled && t >= devices->next_vblank_us) {
        machine->pending |= IRQ_VBLANK;
        devices->next_vblank_us = t + VBLANK_INTERVAL_US;
    }
}

void
devices_init(Devices *devices, Machine *machine)
{
    memset(devices, 0, sizeof(*devices));
    devices->started_at_us = host_monotonic_us();
    net_init(&devices->net);
    machine->host = devices;
    machine->port_read = devices_read_port;
    machine->port_write = devices_write_port;
}

bool
devices_open(Devices *devices, const char *disk_path, uint32_t width, uint32_t height, bool silent,
             uint32_t fixed_date)
{
    devices->fixed_date = fixed_date;
    devices->audio.output = audio_open(silent);
    if (devices->audio.output == NULL) {
        fprintf(stderr, "terrarium: out of memory opening audio\n");
        return false;
    }
    if (!gpu_init(&devices->gpu, width, height)) {
        fprintf(stderr, "terrarium: can't allocate a %ux%u screen\n", width, height);
        return false;
    }
    devices->disk.file = fopen(disk_path, "r+b");
    size_t size = 0;
    devices->disk.bytes = read_file(disk_path, &size);
    if (devices->disk.file == NULL || devices->disk.bytes == NULL) {
        fprintf(stderr, "terrarium: can't open %s\n", disk_path);
        return false;
    }
    if (size / DISK_BLOCK_SIZE > UINT32_MAX) {
        fprintf(stderr, "terrarium: disk is too large\n");
        return false;
    }
    devices->disk.blocks = (uint32_t)(size / DISK_BLOCK_SIZE);
    return true;
}

void
devices_close(Devices *devices)
{
    net_close(&devices->net);
    audio_close(devices->audio.output);
    devices->audio.output = NULL;
    gpu_free(&devices->gpu);
    if (devices->disk.file != NULL)
        fclose(devices->disk.file);
    devices->disk.file = NULL;
    free(devices->disk.bytes);
    devices->disk.bytes = NULL;
}

void
devices_poll(Devices *devices, Machine *machine)
{
    console_poll(machine, devices, 0);
    timer_poll(machine, devices);
    video_poll(machine, devices);
    if (net_poll(&devices->net))
        machine->pending |= IRQ_NET;
}

void
devices_wait(Devices *devices, Machine *machine, int timeout_ms)
{
    if (!devices->console.closed) {
        console_poll(machine, devices, timeout_ms);
    } else if (timeout_ms > 0) {
        struct timespec delay = {
            .tv_sec = timeout_ms / 1000,
            .tv_nsec = (long)(timeout_ms % 1000) * 1000000,
        };
        nanosleep(&delay, NULL);
    }
}

int
devices_timeout(const Devices *devices, uint64_t now_us)
{
    int timeout_ms = host_deadline_ms(devices->timer_at_us, now_us);
    if (devices->vblank_enabled) {
        // The first vertical blank is due immediately after enabling it.
        int vblank_ms =
            devices->next_vblank_us == 0 ? 0 : host_deadline_ms(devices->next_vblank_us, now_us);
        timeout_ms = host_min_timeout(timeout_ms, vblank_ms);
    }
    if (net_active(&devices->net))
        timeout_ms = host_min_timeout(timeout_ms, NETWORK_POLL_MS);
    return timeout_ms;
}
