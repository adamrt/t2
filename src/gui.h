#pragma once

#include "gpu.h"

// Input events, as the input device hands them to the kernel.
enum {
    INPUT_KEY_DOWN = 1,     // a: key, b: modifiers
    INPUT_KEY_UP = 2,
    INPUT_TEXT = 3,         // a: character
    INPUT_MOUSE_MOVE = 4,   // a: x, b: y, c: buttons held
    INPUT_MOUSE_DOWN = 5,   // a: x, b: y, c: button
    INPUT_MOUSE_UP = 6,
    INPUT_WHEEL = 7,        // a: x, b: y, as signed steps
};

typedef struct {
    uint32_t type, a, b, c;
} InputEvent;

#define INPUT_QUEUE 256

typedef struct {
    InputEvent events[INPUT_QUEUE];
    uint32_t start;
    uint32_t count;
} InputQueue;

typedef struct Gui Gui;

// Opens a window and picks the framebuffer size: the largest that fits the
// screen, at most 1280 pixels wide, shown at a whole-number scale.
Gui *gui_open(bool fullscreen, uint32_t *width, uint32_t *height);
void gui_close(Gui *gui);
// Returns false once the window has been closed.
bool gui_poll(Gui *gui, InputQueue *queue);
void gui_wait(Gui *gui, int timeout_ms);
void gui_show(Gui *gui, Gpu *g);
