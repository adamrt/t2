#pragma once

#include "machine.h"

// The video device: two framebuffers in VRAM and a GPU that draws into the
// back one. See docs/devices.md.

enum {
    PORT_VIDEO_WIDTH = 0x40,
    PORT_VIDEO_HEIGHT = 0x41,
    PORT_VIDEO_PRESENT = 0x42,
    PORT_VIDEO_VBLANK = 0x43,
    PORT_GPU_COMMANDS = 0x48,
    PORT_GPU_LENGTH = 0x49,
    PORT_GPU_SOURCE_BASE = 0x4a,
    PORT_GPU_SOURCE_SIZE = 0x4b,
    PORT_GPU_ORIGIN_X = 0x4c,
    PORT_GPU_ORIGIN_Y = 0x4d,
    PORT_GPU_CLIPS = 0x4e,
    PORT_GPU_CLIP_COUNT = 0x4f,
    PORT_GPU_FONT = 0x50,
    PORT_GPU_RUN = 0x51,
    PORT_GPU_STATUS = 0x52,
    PORT_GPU_PIXELS = 0x53,
};

enum {
    GPU_FILL = 1,
    GPU_LINE = 2,
    GPU_BLIT = 3,
    GPU_TEXT = 4,
    GPU_TRIANGLE = 5,
    GPU_TEXTURED = 6,
    GPU_CLIP = 7,
};

#define GPU_MAX_CLIPS 64
// A list stops once it has visited this many screens' worth of pixels.
#define GPU_BUDGET_SCREENS 16

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t *vram;
    uint32_t *back;
    uint32_t *front;
    bool presented; // the front buffer changed

    uint32_t commands; // physical address
    uint32_t length;   // in words
    uint32_t source_base;
    uint32_t source_size;
    int32_t origin_x;
    int32_t origin_y;
    uint32_t clips; // physical address of x, y, w, h rectangles
    uint32_t clip_count;
    uint32_t font; // physical address
    uint32_t status;
    uint32_t pixels; // pixels drawn, for curiosity
} Gpu;

bool gpu_init(Gpu *gpu, uint32_t width, uint32_t height);
void gpu_free(Gpu *gpu);
bool gpu_read(Gpu *gpu, uint32_t port, uint32_t *value);
bool gpu_write(Gpu *gpu, Machine *machine, uint32_t port, uint32_t value);
