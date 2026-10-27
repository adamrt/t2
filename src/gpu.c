// The GPU runs command lists from RAM and draws into the back framebuffer.
// Every coordinate is moved by the origin and clipped to the clip rectangles,
// and images can only come from the source memory the kernel allows. A list
// gets a budget of pixels to visit, so one list can't hold up the machine.

#include "gpu.h"
#include "bytes.h"
#include "utf8.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int32_t x0, y0, x1, y1; // x1 and y1 are just past the edge
} Box;

typedef struct {
    Gpu *gpu;
    Machine *machine;
    Box clips[GPU_MAX_CLIPS];
    uint32_t clip_count;
    Box local;     // the list's own clip rectangle
    uint64_t work; // pixels visited, and steps along lines
} DrawContext;

static int32_t
min_i32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static int32_t
max_i32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

// Coordinates and sizes from a command list are kept small, so no sum or
// product below can overflow, whatever the list says.
#define COORD_LIMIT 32768

static uint64_t
box_area(Box b)
{
    return (uint64_t)(b.x1 - b.x0) * (uint64_t)(b.y1 - b.y0);
}

static int32_t
translate_coord(uint32_t v, int32_t origin)
{
    int64_t x = (int64_t)(int32_t)v + origin;
    return (int32_t)(x < -COORD_LIMIT ? -COORD_LIMIT : x > COORD_LIMIT ? COORD_LIMIT : x);
}

static int32_t
clamp_extent(uint32_t v)
{
    int32_t s = (int32_t)v;
    return s < 0 ? 0 : s > 2 * COORD_LIMIT ? 2 * COORD_LIMIT : s;
}

static Box
intersect(Box a, Box b)
{
    return (Box){ max_i32(a.x0, b.x0), max_i32(a.y0, b.y0), min_i32(a.x1, b.x1),
                  min_i32(a.y1, b.y1) };
}

static bool
empty(Box b)
{
    return b.x0 >= b.x1 || b.y0 >= b.y1;
}

bool
gpu_init(Gpu *gpu, uint32_t width, uint32_t height)
{
    memset(gpu, 0, sizeof(*gpu));
    if ((uint64_t)width * height > VRAM_SIZE / (sizeof(uint32_t) * 2))
        return false;
    gpu->vram = calloc(VRAM_SIZE, 1);
    if (gpu->vram == NULL)
        return false;
    gpu->width = width;
    gpu->height = height;
    gpu->back = (uint32_t *)gpu->vram;
    gpu->front = gpu->back + (size_t)width * height;
    return true;
}

void
gpu_free(Gpu *gpu)
{
    free(gpu->vram);
    memset(gpu, 0, sizeof(*gpu));
}

// Bytes in RAM at a physical address, or NULL.
static const uint8_t *
physical_bytes(Machine *machine, uint32_t addr, uint64_t len)
{
    if (addr > RAM_SIZE || len > RAM_SIZE - addr)
        return NULL;
    return machine->ram + addr;
}

// Bytes at a data address of the program that built the command list.
static const uint8_t *
source_bytes(DrawContext *context, uint32_t addr, uint64_t len)
{
    if (addr < NULL_GUARD)
        return NULL;
    uint64_t off = (uint64_t)addr - NULL_GUARD;
    if (off > context->gpu->source_size || len > context->gpu->source_size - off)
        return NULL;
    return physical_bytes(context->machine, context->gpu->source_base + (uint32_t)off, len);
}

static uint32_t
blend(uint32_t dst, uint32_t src)
{
    uint32_t a = src >> 24;
    if (a == 255)
        return src;
    if (a == 0)
        return dst;
    uint32_t out = 0xff000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        uint32_t s = src >> shift & 255;
        uint32_t d = dst >> shift & 255;
        out |= ((s * a + d * (255 - a) + 127) / 255) << shift;
    }
    return out;
}

static void
plot(DrawContext *context, Box clip, int32_t x, int32_t y, uint32_t color)
{
    if (x < clip.x0 || x >= clip.x1 || y < clip.y0 || y >= clip.y1)
        return;
    uint32_t *p = &context->gpu->back[(size_t)y * context->gpu->width + (size_t)x];
    *p = blend(*p, color);
    context->gpu->pixels++;
}

static void
fill_box(DrawContext *context, Box box, uint32_t color)
{
    for (uint32_t i = 0; i < context->clip_count; i++) {
        Box b = intersect(intersect(box, context->clips[i]), context->local);
        if (empty(b))
            continue;
        context->work += box_area(b);
        for (int32_t y = b.y0; y < b.y1; y++) {
            uint32_t *row = &context->gpu->back[(size_t)y * context->gpu->width];
            for (int32_t x = b.x0; x < b.x1; x++)
                row[x] = blend(row[x], color);
        }
        context->gpu->pixels += (uint32_t)box_area(b);
    }
}

static void
draw_line(DrawContext *context, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color)
{
    int32_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int32_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    for (uint32_t i = 0; i < context->clip_count; i++) {
        Box clip = intersect(context->clips[i], context->local);
        if (empty(clip))
            continue;
        context->work += (uint64_t)max_i32(dx, -dy) + 1;
        int32_t x = x0, y = y0, err = dx + dy;
        for (;;) {
            plot(context, clip, x, y, color);
            if (x == x1 && y == y1)
                break;
            int32_t e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y += sy;
            }
        }
    }
}

static bool
draw_image(DrawContext *context, const uint32_t *args)
{
    uint32_t image = args[0], width = args[1], height = args[2];
    int32_t sx = translate_coord(args[3], 0), sy = translate_coord(args[4], 0),
            bw = clamp_extent(args[5]), bh = clamp_extent(args[6]);
    int32_t dx = translate_coord(args[7], context->gpu->origin_x),
            dy = translate_coord(args[8], context->gpu->origin_y);
    uint32_t mode = args[9];
    if (width > 2 * COORD_LIMIT || height > 2 * COORD_LIMIT || mode > 1)
        return false;
    const uint8_t *pixels = source_bytes(context, image, (uint64_t)width * height * 4);
    if (pixels == NULL)
        return false;
    // Keep the source rectangle inside the image.
    if (sx < 0) {
        dx -= sx;
        bw += sx;
        sx = 0;
    }
    if (sy < 0) {
        dy -= sy;
        bh += sy;
        sy = 0;
    }
    bw = min_i32(bw, (int32_t)width - sx);
    bh = min_i32(bh, (int32_t)height - sy);
    Box dest = { dx, dy, dx + bw, dy + bh };
    for (uint32_t i = 0; i < context->clip_count; i++) {
        Box b = intersect(intersect(dest, context->clips[i]), context->local);
        if (empty(b))
            continue;
        context->work += box_area(b);
        for (int32_t y = b.y0; y < b.y1; y++) {
            uint32_t *row = &context->gpu->back[(size_t)y * context->gpu->width];
            const uint8_t *src =
                pixels + ((size_t)(sy + y - dy) * width + (size_t)(sx + b.x0 - dx)) * 4;
            for (int32_t x = b.x0; x < b.x1; x++, src += 4) {
                uint32_t s = read_u32_le(src);
                row[x] = mode == 0 ? (s | 0xff000000u) : blend(row[x], s);
            }
        }
        context->gpu->pixels += (uint32_t)box_area(b);
    }
    return true;
}

static bool
draw_text(DrawContext *context, const uint32_t *args)
{
    int32_t x = translate_coord(args[0], context->gpu->origin_x),
            y = translate_coord(args[1], context->gpu->origin_y);
    uint32_t color = args[2], length = args[4], scale = args[5];
    const uint8_t *chars = source_bytes(context, args[3], length);
    const uint8_t *font = physical_bytes(context->machine, context->gpu->font, 20);
    if (chars == NULL || font == NULL || memcmp(font, "TFN1", 4) != 0 || scale == 0 || scale > 16)
        return false;
    uint32_t fw = read_u32_le(font + 4), fh = read_u32_le(font + 8), first = read_u32_le(font + 12),
             count = read_u32_le(font + 16);
    const uint8_t *glyphs =
        physical_bytes(context->machine, context->gpu->font + 20, (uint64_t)count * fh);
    if (glyphs == NULL || fw > 8 || count == 0)
        return false;
    for (const uint8_t *p = chars, *end = chars + length;
         p < end && x < (int32_t)context->gpu->width; x += (int32_t)(fw * scale)) {
        uint32_t ch = utf8_next(&p, end);
        if (ch < first || ch >= first + count)
            ch = '?' >= first && '?' < first + count ? '?' : first;
        const uint8_t *glyph = glyphs + (ch - first) * fh;
        context->work += fw * fh;
        for (uint32_t row = 0; row < fh; row++) {
            for (uint32_t col = 0; col < fw; col++) {
                if (glyph[row] & (0x80 >> col)) {
                    int32_t px = x + (int32_t)(col * scale), py = y + (int32_t)(row * scale);
                    fill_box(context, (Box){ px, py, px + (int32_t)scale, py + (int32_t)scale },
                             color);
                }
            }
        }
    }
    return true;
}

typedef struct {
    int32_t x, y;
    uint32_t color;
    int32_t u, v;
} Vertex;

static int64_t
edge_function(const Vertex *a, const Vertex *b, int32_t px, int32_t py)
{
    return (int64_t)(b->x - a->x) * (py - a->y) - (int64_t)(b->y - a->y) * (px - a->x);
}

static uint32_t
interpolate_color(uint32_t c0, uint32_t c1, uint32_t c2, int64_t w0, int64_t w1, int64_t w2,
                  int64_t area)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        int64_t v =
            (w0 * (c0 >> shift & 255) + w1 * (c1 >> shift & 255) + w2 * (c2 >> shift & 255)) / area;
        out |= (uint32_t)v << shift;
    }
    return out;
}

static uint32_t
modulate(uint32_t texel, uint32_t tint)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8)
        out |= ((texel >> shift & 255) * (tint >> shift & 255) / 255) << shift;
    return out;
}

// Fills a triangle, mixing the vertex colors, or sampling the texture when
// there is one. Texture coordinates are interpolated without perspective.
static void
draw_triangle(DrawContext *context, Vertex v[3], const uint8_t *texture, uint32_t tw, uint32_t th,
              uint32_t tint)
{
    int64_t area = edge_function(&v[0], &v[1], v[2].x, v[2].y);
    if (area == 0)
        return;
    if (area < 0) {
        Vertex t = v[1];
        v[1] = v[2];
        v[2] = t;
        area = -area;
    }
    Box bounds = {
        min_i32(v[0].x, min_i32(v[1].x, v[2].x)),
        min_i32(v[0].y, min_i32(v[1].y, v[2].y)),
        max_i32(v[0].x, max_i32(v[1].x, v[2].x)) + 1,
        max_i32(v[0].y, max_i32(v[1].y, v[2].y)) + 1,
    };
    for (uint32_t i = 0; i < context->clip_count; i++) {
        Box b = intersect(intersect(bounds, context->clips[i]), context->local);
        if (empty(b))
            continue;
        context->work += box_area(b);
        for (int32_t y = b.y0; y < b.y1; y++) {
            uint32_t *row = &context->gpu->back[(size_t)y * context->gpu->width];
            for (int32_t x = b.x0; x < b.x1; x++) {
                int64_t w0 = edge_function(&v[1], &v[2], x, y);
                int64_t w1 = edge_function(&v[2], &v[0], x, y);
                int64_t w2 = edge_function(&v[0], &v[1], x, y);
                if (w0 < 0 || w1 < 0 || w2 < 0)
                    continue;
                uint32_t color;
                if (texture == NULL) {
                    color = interpolate_color(v[0].color, v[1].color, v[2].color, w0, w1, w2, area);
                } else {
                    int64_t u = (w0 * v[0].u + w1 * v[1].u + w2 * v[2].u) / area;
                    int64_t t = (w0 * v[0].v + w1 * v[1].v + w2 * v[2].v) / area;
                    u %= tw;
                    t %= th;
                    if (u < 0)
                        u += tw;
                    if (t < 0)
                        t += th;
                    color = modulate(read_u32_le(texture + ((size_t)t * tw + (size_t)u) * 4), tint);
                }
                row[x] = blend(row[x], color);
                context->gpu->pixels++;
            }
        }
    }
}

static uint32_t
command_words(uint32_t op)
{
    switch (op) {
    case GPU_FILL:
        return 6;
    case GPU_LINE:
        return 6;
    case GPU_BLIT:
        return 11;
    case GPU_TEXT:
        return 7;
    case GPU_TRIANGLE:
        return 10;
    case GPU_TEXTURED:
        return 17;
    case GPU_CLIP:
        return 5;
    }
    return 0;
}

// Returns 0, or 1 + the word index of the command that failed.
static uint32_t
run_commands(Gpu *gpu, Machine *machine)
{
    DrawContext context = { .gpu = gpu, .machine = machine };
    Box screen = { 0, 0, (int32_t)gpu->width, (int32_t)gpu->height };
    context.local = screen;
    const uint8_t *clips = physical_bytes(machine, gpu->clips, (uint64_t)gpu->clip_count * 16);
    if (clips == NULL || gpu->clip_count > GPU_MAX_CLIPS)
        return 1;
    for (uint32_t i = 0; i < gpu->clip_count; i++) {
        const uint8_t *r = clips + i * 16;
        int32_t x = translate_coord(read_u32_le(r), 0), y = translate_coord(read_u32_le(r + 4), 0);
        Box b = { x, y, x + clamp_extent(read_u32_le(r + 8)),
                  y + clamp_extent(read_u32_le(r + 12)) };
        context.clips[context.clip_count++] = intersect(b, screen);
    }
    const uint8_t *list = physical_bytes(machine, gpu->commands, (uint64_t)gpu->length * 4);
    if (list == NULL)
        return 1;
    uint64_t budget = GPU_BUDGET_SCREENS * box_area(screen);

    for (uint32_t at = 0; at < gpu->length;) {
        uint32_t op = read_u32_le(list + at * 4);
        uint32_t n = command_words(op);
        if (n == 0 || at + n > gpu->length)
            return at + 1;
        uint32_t args[17];
        for (uint32_t i = 1; i < n; i++)
            args[i - 1] = read_u32_le(list + (at + i) * 4);
        int32_t ox = gpu->origin_x, oy = gpu->origin_y;
        bool ok = true;
        switch (op) {
        case GPU_FILL: {
            int32_t x = translate_coord(args[0], ox), y = translate_coord(args[1], oy);
            fill_box(&context, (Box){ x, y, x + clamp_extent(args[2]), y + clamp_extent(args[3]) },
                     args[4]);
            break;
        }
        case GPU_LINE:
            draw_line(&context, translate_coord(args[0], ox), translate_coord(args[1], oy),
                      translate_coord(args[2], ox), translate_coord(args[3], oy), args[4]);
            break;
        case GPU_BLIT:
            ok = draw_image(&context, args);
            break;
        case GPU_TEXT:
            ok = draw_text(&context, args);
            break;
        case GPU_TRIANGLE: {
            Vertex v[3];
            for (int i = 0; i < 3; i++)
                v[i] = (Vertex){ translate_coord(args[i * 3], ox),
                                 translate_coord(args[i * 3 + 1], oy), args[i * 3 + 2], 0, 0 };
            draw_triangle(&context, v, NULL, 0, 0, 0);
            break;
        }
        case GPU_TEXTURED: {
            uint32_t tw = args[1], th = args[2];
            const uint8_t *texture = tw > 2 * COORD_LIMIT || th > 2 * COORD_LIMIT
                                         ? NULL
                                         : source_bytes(&context, args[0], (uint64_t)tw * th * 4);
            if (texture == NULL || tw == 0 || th == 0) {
                ok = false;
                break;
            }
            Vertex v[3];
            for (int i = 0; i < 3; i++)
                v[i] = (Vertex){ translate_coord(args[3 + i * 4], ox),
                                 translate_coord(args[4 + i * 4], oy), 0,
                                 translate_coord(args[5 + i * 4], 0),
                                 translate_coord(args[6 + i * 4], 0) };
            draw_triangle(&context, v, texture, tw, th, args[15]);
            break;
        }
        case GPU_CLIP:
            if ((int32_t)args[2] < 0 || (int32_t)args[3] < 0) {
                context.local = screen;
            } else {
                int32_t x = translate_coord(args[0], ox), y = translate_coord(args[1], oy);
                context.local = intersect(
                    (Box){ x, y, x + clamp_extent(args[2]), y + clamp_extent(args[3]) }, screen);
            }
            break;
        }
        if (!ok || context.work > budget)
            return at + 1;
        at += n;
    }
    return 0;
}

bool
gpu_read(Gpu *gpu, uint32_t port, uint32_t *value)
{
    switch (port) {
    case PORT_VIDEO_WIDTH:
        *value = gpu->width;
        return true;
    case PORT_VIDEO_HEIGHT:
        *value = gpu->height;
        return true;
    case PORT_GPU_STATUS:
        *value = gpu->status;
        return true;
    case PORT_GPU_PIXELS:
        *value = gpu->pixels;
        return true;
    }
    return false;
}

bool
gpu_write(Gpu *gpu, Machine *machine, uint32_t port, uint32_t value)
{
    switch (port) {
    case PORT_VIDEO_PRESENT:
        if (gpu->width != 0) {
            memcpy(gpu->front, gpu->back, (size_t)gpu->width * gpu->height * 4);
            gpu->presented = true;
        }
        return true;
    case PORT_GPU_COMMANDS:
        gpu->commands = value;
        return true;
    case PORT_GPU_LENGTH:
        gpu->length = value;
        return true;
    case PORT_GPU_SOURCE_BASE:
        gpu->source_base = value;
        return true;
    case PORT_GPU_SOURCE_SIZE:
        gpu->source_size = value;
        return true;
    case PORT_GPU_ORIGIN_X:
        gpu->origin_x = (int32_t)value;
        return true;
    case PORT_GPU_ORIGIN_Y:
        gpu->origin_y = (int32_t)value;
        return true;
    case PORT_GPU_CLIPS:
        gpu->clips = value;
        return true;
    case PORT_GPU_CLIP_COUNT:
        gpu->clip_count = value;
        return true;
    case PORT_GPU_FONT:
        gpu->font = value;
        return true;
    case PORT_GPU_RUN:
        gpu->status = gpu->width == 0 ? 1 : run_commands(gpu, machine);
        return true;
    }
    return false;
}
