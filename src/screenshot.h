#pragma once

#include "gpu.h"

// Write the front framebuffer as an uncompressed PNG.
bool screenshot_write(const Gpu *gpu, const char *path);
