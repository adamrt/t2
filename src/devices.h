#pragma once

#include "audio.h"
#include "gpu.h"
#include "input.h"
#include "net.h"

#include <stdio.h>

#define DISK_BLOCK_SIZE 4096

typedef struct {
    uint8_t bytes[4096];
    size_t start;
    size_t end;
    bool closed;
} ConsoleDevice;

typedef struct {
    FILE *file;
    uint8_t *bytes;
    uint32_t blocks;
    uint32_t block;
    uint32_t address;
    uint32_t count;
    uint32_t status;
} DiskDevice;

typedef struct {
    Audio *output;
    uint32_t address;
    uint32_t length;
    uint32_t status;
} AudioDevice;

typedef struct {
    ConsoleDevice console;
    DiskDevice disk;
    AudioDevice audio;
    Gpu gpu;
    Net net;
    InputQueue input;
    uint64_t started_at_us;
    uint64_t timer_at_us; // 0 when cancelled.
    uint64_t next_vblank_us;
    bool vblank_enabled;
    uint32_t fixed_date;
} Devices;

// Initialize before opening resources; close is safe after any open failure.
void devices_init(Devices *devices, Machine *machine);
bool devices_open(Devices *devices, const char *disk_path, uint32_t width, uint32_t height,
                  bool silent, uint32_t fixed_date);
void devices_close(Devices *devices);
void devices_poll(Devices *devices, Machine *machine);
void devices_wait(Devices *devices, Machine *machine, int timeout_ms);
// Milliseconds until the next device deadline, or -1 when none is scheduled.
int devices_timeout(const Devices *devices, uint64_t now_us);
