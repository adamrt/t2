// The GPU runs command lists from RAM and draws into the back framebuffer.
// Every coordinate is moved by the origin and clipped to the clip rectangles,
// and images can only come from the source memory the kernel allows. A list
// gets a budget of pixels to visit, so one list can't hold up the machine.

#include "gpu.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int32_t x0, y0, x1, y1; // x1 and y1 are just past the edge
} Box;

typedef struct {
    Gpu *g;
    Machine *m;
    Box clips[GPU_MAX_CLIPS];
    uint32_t clip_count;
    Box local;              // the list's own clip rectangle
    uint64_t work;          // pixels visited, and steps along lines
} Context;

static uint32_t
word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int32_t
min32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static int32_t
max32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

// Coordinates and sizes from a command list are kept small, so no sum or
// product below can overflow, whatever the list says.
#define LIMIT 32768

static uint64_t
box_area(Box b)
{
    return (uint64_t)(b.x1 - b.x0) * (uint64_t)(b.y1 - b.y0);
}

static int32_t
coord(uint32_t v, int32_t origin)
{
    int64_t x = (int64_t)(int32_t)v + origin;
    return (int32_t)(x < -LIMIT ? -LIMIT : x > LIMIT ? LIMIT : x);
}

static int32_t
extent(uint32_t v)
{
    int32_t s = (int32_t)v;
    return s < 0 ? 0 : s > 2 * LIMIT ? 2 * LIMIT : s;
}

static Box
intersect(Box a, Box b)
{
    return (Box){ max32(a.x0, b.x0), max32(a.y0, b.y0), min32(a.x1, b.x1), min32(a.y1, b.y1) };
}

static bool
empty(Box b)
{
    return b.x0 >= b.x1 || b.y0 >= b.y1;
}

bool
gpu_init(Gpu *g, uint32_t width, uint32_t height)
{
    memset(g, 0, sizeof(*g));
    if ((uint64_t)width * height * 4 * 2 > VRAM_SIZE)
        return false;
    g->vram = calloc(VRAM_SIZE, 1);
    if (g->vram == NULL)
        return false;
    g->width = width;
    g->height = height;
    g->back = (uint32_t *)g->vram;
    g->front = g->back + (size_t)width * height;
    return true;
}

// Bytes in RAM at a physical address, or NULL.
static const uint8_t *
physical(Machine *m, uint32_t addr, uint64_t len)
{
    if (addr > RAM_SIZE || len > RAM_SIZE - addr)
        return NULL;
    return m->ram + addr;
}

// Bytes at a data address of the program that built the command list.
static const uint8_t *
source(Context *c, uint32_t addr, uint64_t len)
{
    if (addr < NULL_GUARD)
        return NULL;
    uint64_t off = (uint64_t)addr - NULL_GUARD;
    if (off > c->g->source_size || len > c->g->source_size - off)
        return NULL;
    return physical(c->m, c->g->source_base + (uint32_t)off, len);
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
plot(Context *c, Box clip, int32_t x, int32_t y, uint32_t color)
{
    if (x < clip.x0 || x >= clip.x1 || y < clip.y0 || y >= clip.y1)
        return;
    uint32_t *p = &c->g->back[(size_t)y * c->g->width + (size_t)x];
    *p = blend(*p, color);
    c->g->pixels++;
}

static void
fill_box(Context *c, Box box, uint32_t color)
{
    for (uint32_t i = 0; i < c->clip_count; i++) {
        Box b = intersect(intersect(box, c->clips[i]), c->local);
        if (empty(b))
            continue;
        c->work += box_area(b);
        for (int32_t y = b.y0; y < b.y1; y++) {
            uint32_t *row = &c->g->back[(size_t)y * c->g->width];
            for (int32_t x = b.x0; x < b.x1; x++)
                row[x] = blend(row[x], color);
        }
        c->g->pixels += (uint32_t)box_area(b);
    }
}

static void
draw_line(Context *c, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color)
{
    int32_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int32_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    for (uint32_t i = 0; i < c->clip_count; i++) {
        Box clip = intersect(c->clips[i], c->local);
        if (empty(clip))
            continue;
        c->work += (uint64_t)max32(dx, -dy) + 1;
        int32_t x = x0, y = y0, err = dx + dy;
        for (;;) {
            plot(c, clip, x, y, color);
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
blit(Context *c, const uint32_t *w)
{
    uint32_t image = w[0], width = w[1], height = w[2];
    int32_t sx = coord(w[3], 0), sy = coord(w[4], 0), bw = extent(w[5]), bh = extent(w[6]);
    int32_t dx = coord(w[7], c->g->origin_x), dy = coord(w[8], c->g->origin_y);
    uint32_t mode = w[9];
    if (width > 2 * LIMIT || height > 2 * LIMIT || mode > 1)
        return false;
    const uint8_t *pixels = source(c, image, (uint64_t)width * height * 4);
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
    bw = min32(bw, (int32_t)width - sx);
    bh = min32(bh, (int32_t)height - sy);
    Box dest = { dx, dy, dx + bw, dy + bh };
    for (uint32_t i = 0; i < c->clip_count; i++) {
        Box b = intersect(intersect(dest, c->clips[i]), c->local);
        if (empty(b))
            continue;
        c->work += box_area(b);
        for (int32_t y = b.y0; y < b.y1; y++) {
            uint32_t *row = &c->g->back[(size_t)y * c->g->width];
            const uint8_t *src = pixels + ((size_t)(sy + y - dy) * width + (size_t)(sx + b.x0 - dx)) * 4;
            for (int32_t x = b.x0; x < b.x1; x++, src += 4) {
                uint32_t s = word(src);
                row[x] = mode == 0 ? (s | 0xff000000u) : blend(row[x], s);
            }
        }
        c->g->pixels += (uint32_t)box_area(b);
    }
    return true;
}

uint32_t
utf8_next(const uint8_t **s, const uint8_t *end)
{
    const uint8_t *p = *s;
    uint32_t b = *p, n = b >= 0xf0 && b < 0xf5 ? 3 : b >= 0xe0 ? 2 : b >= 0xc2 && b < 0xe0 ? 1 : 0;
    *s = p + 1;
    if (b < 0x80)
        return b;
    if (n == 0 || b >= 0xf5 || (ptrdiff_t)n >= end - p)
        return 0xfffd;
    uint32_t ch = b & (0x3fu >> n);
    for (uint32_t k = 1; k <= n; k++) {
        if ((p[k] & 0xc0) != 0x80)
            return 0xfffd;
        ch = ch << 6 | (p[k] & 0x3fu);
    }
    *s = p + n + 1;
    return ch;
}

static bool
text(Context *c, const uint32_t *w)
{
    int32_t x = coord(w[0], c->g->origin_x), y = coord(w[1], c->g->origin_y);
    uint32_t color = w[2], length = w[4], scale = w[5];
    const uint8_t *chars = source(c, w[3], length);
    const uint8_t *font = physical(c->m, c->g->font, 20);
    if (chars == NULL || font == NULL || memcmp(font, "TFN1", 4) != 0 || scale == 0 || scale > 16)
        return false;
    uint32_t fw = word(font + 4), fh = word(font + 8), first = word(font + 12), count = word(font + 16);
    const uint8_t *glyphs = physical(c->m, c->g->font + 20, (uint64_t)count * fh);
    if (glyphs == NULL || fw > 8 || count == 0)
        return false;
    for (const uint8_t *p = chars, *end = chars + length; p < end && x < (int32_t)c->g->width; x += (int32_t)(fw * scale)) {
        uint32_t ch = utf8_next(&p, end);
        if (ch < first || ch >= first + count)
            ch = '?' >= first && '?' < first + count ? '?' : first;
        const uint8_t *glyph = glyphs + (ch - first) * fh;
        c->work += fw * fh;
        for (uint32_t row = 0; row < fh; row++) {
            for (uint32_t col = 0; col < fw; col++) {
                if (glyph[row] & (0x80 >> col)) {
                    int32_t px = x + (int32_t)(col * scale), py = y + (int32_t)(row * scale);
                    fill_box(c, (Box){ px, py, px + (int32_t)scale, py + (int32_t)scale }, color);
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
edge(const Vertex *a, const Vertex *b, int32_t px, int32_t py)
{
    return (int64_t)(b->x - a->x) * (py - a->y) - (int64_t)(b->y - a->y) * (px - a->x);
}

static uint32_t
mix(uint32_t c0, uint32_t c1, uint32_t c2, int64_t w0, int64_t w1, int64_t w2, int64_t area)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        int64_t v = (w0 * (c0 >> shift & 255) + w1 * (c1 >> shift & 255) + w2 * (c2 >> shift & 255)) / area;
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
triangle(Context *c, Vertex v[3], const uint8_t *texture, uint32_t tw, uint32_t th, uint32_t tint)
{
    int64_t area = edge(&v[0], &v[1], v[2].x, v[2].y);
    if (area == 0)
        return;
    if (area < 0) {
        Vertex t = v[1];
        v[1] = v[2];
        v[2] = t;
        area = -area;
    }
    Box bounds = {
        min32(v[0].x, min32(v[1].x, v[2].x)), min32(v[0].y, min32(v[1].y, v[2].y)),
        max32(v[0].x, max32(v[1].x, v[2].x)) + 1, max32(v[0].y, max32(v[1].y, v[2].y)) + 1,
    };
    for (uint32_t i = 0; i < c->clip_count; i++) {
        Box b = intersect(intersect(bounds, c->clips[i]), c->local);
        if (empty(b))
            continue;
        c->work += box_area(b);
        for (int32_t y = b.y0; y < b.y1; y++) {
            uint32_t *row = &c->g->back[(size_t)y * c->g->width];
            for (int32_t x = b.x0; x < b.x1; x++) {
                int64_t w0 = edge(&v[1], &v[2], x, y);
                int64_t w1 = edge(&v[2], &v[0], x, y);
                int64_t w2 = edge(&v[0], &v[1], x, y);
                if (w0 < 0 || w1 < 0 || w2 < 0)
                    continue;
                uint32_t color;
                if (texture == NULL) {
                    color = mix(v[0].color, v[1].color, v[2].color, w0, w1, w2, area);
                } else {
                    int64_t u = (w0 * v[0].u + w1 * v[1].u + w2 * v[2].u) / area;
                    int64_t t = (w0 * v[0].v + w1 * v[1].v + w2 * v[2].v) / area;
                    u %= tw;
                    t %= th;
                    if (u < 0)
                        u += tw;
                    if (t < 0)
                        t += th;
                    color = modulate(word(texture + ((size_t)t * tw + (size_t)u) * 4), tint);
                }
                row[x] = blend(row[x], color);
                c->g->pixels++;
            }
        }
    }
}

static uint32_t
command_words(uint32_t op)
{
    switch (op) {
    case GPU_FILL:     return 6;
    case GPU_LINE:     return 6;
    case GPU_BLIT:     return 11;
    case GPU_TEXT:     return 7;
    case GPU_TRIANGLE: return 10;
    case GPU_TEXTURED: return 17;
    case GPU_CLIP:     return 5;
    }
    return 0;
}

// Returns 0, or 1 + the word index of the command that failed.
static uint32_t
run(Gpu *g, Machine *m)
{
    Context c = { .g = g, .m = m };
    Box screen = { 0, 0, (int32_t)g->width, (int32_t)g->height };
    c.local = screen;
    const uint8_t *clips = physical(m, g->clips, (uint64_t)g->clip_count * 16);
    if (clips == NULL || g->clip_count > GPU_MAX_CLIPS)
        return 1;
    for (uint32_t i = 0; i < g->clip_count; i++) {
        const uint8_t *r = clips + i * 16;
        int32_t x = coord(word(r), 0), y = coord(word(r + 4), 0);
        Box b = { x, y, x + extent(word(r + 8)), y + extent(word(r + 12)) };
        c.clips[c.clip_count++] = intersect(b, screen);
    }
    const uint8_t *list = physical(m, g->commands, (uint64_t)g->length * 4);
    if (list == NULL)
        return 1;
    uint64_t budget = GPU_BUDGET_SCREENS * box_area(screen);

    for (uint32_t at = 0; at < g->length;) {
        uint32_t op = word(list + at * 4);
        uint32_t n = command_words(op);
        if (n == 0 || at + n > g->length)
            return at + 1;
        uint32_t w[17];
        for (uint32_t i = 1; i < n; i++)
            w[i - 1] = word(list + (at + i) * 4);
        int32_t ox = g->origin_x, oy = g->origin_y;
        bool ok = true;
        switch (op) {
        case GPU_FILL: {
            int32_t x = coord(w[0], ox), y = coord(w[1], oy);
            fill_box(&c, (Box){ x, y, x + extent(w[2]), y + extent(w[3]) }, w[4]);
            break;
        }
        case GPU_LINE:
            draw_line(&c, coord(w[0], ox), coord(w[1], oy), coord(w[2], ox), coord(w[3], oy), w[4]);
            break;
        case GPU_BLIT:
            ok = blit(&c, w);
            break;
        case GPU_TEXT:
            ok = text(&c, w);
            break;
        case GPU_TRIANGLE: {
            Vertex v[3];
            for (int i = 0; i < 3; i++)
                v[i] = (Vertex){ coord(w[i * 3], ox), coord(w[i * 3 + 1], oy), w[i * 3 + 2], 0, 0 };
            triangle(&c, v, NULL, 0, 0, 0);
            break;
        }
        case GPU_TEXTURED: {
            uint32_t tw = w[1], th = w[2];
            const uint8_t *texture = tw > 2 * LIMIT || th > 2 * LIMIT ? NULL : source(&c, w[0], (uint64_t)tw * th * 4);
            if (texture == NULL || tw == 0 || th == 0) {
                ok = false;
                break;
            }
            Vertex v[3];
            for (int i = 0; i < 3; i++)
                v[i] = (Vertex){ coord(w[3 + i * 4], ox), coord(w[4 + i * 4], oy), 0, coord(w[5 + i * 4], 0), coord(w[6 + i * 4], 0) };
            triangle(&c, v, texture, tw, th, w[15]);
            break;
        }
        case GPU_CLIP:
            if ((int32_t)w[2] < 0 || (int32_t)w[3] < 0) {
                c.local = screen;
            } else {
                int32_t x = coord(w[0], ox), y = coord(w[1], oy);
                c.local = intersect((Box){ x, y, x + extent(w[2]), y + extent(w[3]) }, screen);
            }
            break;
        }
        if (!ok || c.work > budget)
            return at + 1;
        at += n;
    }
    return 0;
}

bool
gpu_read(Gpu *g, uint32_t port, uint32_t *value)
{
    switch (port) {
    case PORT_VIDEO_WIDTH:  *value = g->width; return true;
    case PORT_VIDEO_HEIGHT: *value = g->height; return true;
    case PORT_GPU_STATUS:   *value = g->status; return true;
    case PORT_GPU_PIXELS:   *value = g->pixels; return true;
    }
    return false;
}

bool
gpu_write(Gpu *g, Machine *m, uint32_t port, uint32_t value)
{
    switch (port) {
    case PORT_VIDEO_PRESENT:
        if (g->width != 0) {
            memcpy(g->front, g->back, (size_t)g->width * g->height * 4);
            g->presented = true;
        }
        return true;
    case PORT_GPU_COMMANDS:    g->commands = value; return true;
    case PORT_GPU_LENGTH:      g->length = value; return true;
    case PORT_GPU_SOURCE_BASE: g->source_base = value; return true;
    case PORT_GPU_SOURCE_SIZE: g->source_size = value; return true;
    case PORT_GPU_ORIGIN_X:    g->origin_x = (int32_t)value; return true;
    case PORT_GPU_ORIGIN_Y:    g->origin_y = (int32_t)value; return true;
    case PORT_GPU_CLIPS:       g->clips = value; return true;
    case PORT_GPU_CLIP_COUNT:  g->clip_count = value; return true;
    case PORT_GPU_FONT:        g->font = value; return true;
    case PORT_GPU_RUN:
        g->status = g->width == 0 ? 1 : run(g, m);
        return true;
    }
    return false;
}
