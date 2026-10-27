#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *disk_path;
    const char *symbol_path;
    bool window;
    bool fullscreen;
    bool mute;
    uint32_t screen_width; // A screen without a window, for testing.
    uint32_t screen_height;
    uint32_t run_for_ms; // Stop after this duration; 0 runs indefinitely.
    const char *screenshot_path;
    const char *typed_text;
    int32_t drag[4]; // x0, y0, x1, y1, after the typing.
    bool has_drag;
    uint32_t fixed_date; // Local seconds since 1970; 0 uses the host clock.
} BootOptions;

int boot(const BootOptions *options);
