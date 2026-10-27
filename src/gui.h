#pragma once

#include "gpu.h"
#include "input.h"

typedef struct Gui Gui;

// Opens a window and picks the framebuffer size: the largest that fits the
// screen, at most 1280 pixels wide, shown at a whole-number scale.
Gui *gui_open(bool fullscreen, uint32_t *width, uint32_t *height);
void gui_close(Gui *gui);
// Returns false once the window has been closed.
bool gui_poll(Gui *gui, InputQueue *queue);
void gui_wait(Gui *gui, int timeout_ms);
void gui_show(Gui *gui, Gpu *g);
