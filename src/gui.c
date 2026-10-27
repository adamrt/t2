// The host window: it shows the front framebuffer, draws the mouse cursor,
// and turns the host's keyboard and mouse into input events.

#include "gui.h"
#include "utf8.h"

#include <stdio.h>
#include <string.h>

#ifdef HEADLESS

// Built without SDL there is no window, and the machine runs only with
// --headless or --screen. The other calls happen only with a window.

Gui *
gui_open(bool fullscreen, uint32_t *width, uint32_t *height)
{
    (void)fullscreen;
    (void)width;
    (void)height;
    fprintf(stderr, "terrarium: built without a window; use boot --headless or --screen WxH\n");
    return NULL;
}

void
gui_close(Gui *gui)
{
    (void)gui;
}

bool
gui_poll(Gui *gui, InputQueue *queue)
{
    (void)gui;
    (void)queue;
    return false;
}

void
gui_wait(Gui *gui, int timeout_ms)
{
    (void)gui;
    (void)timeout_ms;
}

void
gui_show(Gui *gui, Gpu *gpu)
{
    (void)gui;
    (void)gpu;
}

#else

#include <SDL3/SDL.h>

struct Gui {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *screen;
    SDL_Texture *cursor;
    float mouse_x;
    float mouse_y;
    bool mouse_inside;
    bool dirty;
    uint64_t last_show;
};

// The cursor, an arrow: '#' is black, '.' is white.
static const char *cursor_art[] = {
    "#           ", "##          ", "#.#         ", "#..#        ", "#...#       ", "#....#      ",
    "#.....#     ", "#......#    ", "#.......#   ", "#........#  ", "#.....#####",  "#..#..#     ",
    "#.# #..#    ", "##  #..#    ", "#    #..#   ", "     #..#   ", "      ##    ",
};

#define CURSOR_W 12
#define CURSOR_H 17
#define MAX_SCREEN_WIDTH 1280
#define FRAME_INTERVAL_MS 16

// The framebuffer is the largest that fits, at most 1280 pixels wide, so
// that a whole number of host pixels shows each of its pixels.
static uint32_t
pick_size(uint32_t pixels_w, uint32_t pixels_h, uint32_t *width, uint32_t *height)
{
    uint32_t scale = pixels_w / MAX_SCREEN_WIDTH + (pixels_w % MAX_SCREEN_WIDTH != 0);
    if (scale == 0)
        scale = 1;
    *width = pixels_w / scale;
    *height = pixels_h / scale;
    if ((uint64_t)*width * *height * 8 > VRAM_SIZE)
        *height = (uint32_t)(VRAM_SIZE / 8 / *width);
    return scale;
}

Gui *
gui_open(bool fullscreen, uint32_t *width, uint32_t *height)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "terrarium: can't start video: %s\n", SDL_GetError());
        return NULL;
    }
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    SDL_Rect area;
    if (!SDL_GetDisplayUsableBounds(display, &area)) {
        area.w = 1280;
        area.h = 800;
    }
    const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(display);
    float density = mode != NULL ? mode->pixel_density : 1;
    if (density < 1)
        density = 1;

    Gui *gui = SDL_calloc(1, sizeof(*gui));
    if (gui == NULL) {
        fprintf(stderr, "terrarium: out of memory opening a window\n");
        SDL_Quit();
        return NULL;
    }
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (fullscreen) {
        // A fullscreen window can be smaller than the display, as on Macs
        // with a notch, so the framebuffer is fitted to the window itself.
        gui->window = SDL_CreateWindow("Terrarium", area.w, area.h, flags | SDL_WINDOW_FULLSCREEN);
        int pixels_w = 0, pixels_h = 0;
        if (gui->window != NULL) {
            SDL_SyncWindow(gui->window);
            SDL_GetWindowSizeInPixels(gui->window, &pixels_w, &pixels_h);
        }
        pick_size((uint32_t)pixels_w, (uint32_t)pixels_h, width, height);
    } else {
        area.h -= 32; // room for the window's title bar
        uint32_t scale = pick_size((uint32_t)((float)area.w * density),
                                   (uint32_t)((float)area.h * density), width, height);
        int window_w = (int)((float)(*width * scale) / density);
        int window_h = (int)((float)(*height * scale) / density);
        gui->window = SDL_CreateWindow("Terrarium", window_w, window_h, flags);
    }
    gui->renderer = gui->window ? SDL_CreateRenderer(gui->window, NULL) : NULL;
    if (gui->renderer == NULL || *width == 0 || *height == 0) {
        fprintf(stderr, "terrarium: can't open a window: %s\n", SDL_GetError());
        gui_close(gui);
        return NULL;
    }
    SDL_SetRenderLogicalPresentation(gui->renderer, (int)*width, (int)*height,
                                     SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    gui->screen = SDL_CreateTexture(gui->renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, (int)*width, (int)*height);
    if (gui->screen == NULL)
        goto texture_error;
    SDL_SetTextureScaleMode(gui->screen, SDL_SCALEMODE_NEAREST);

    uint32_t art[CURSOR_W * CURSOR_H];
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            char c = x < (int)SDL_strlen(cursor_art[y]) ? cursor_art[y][x] : ' ';
            art[y * CURSOR_W + x] = c == '#' ? 0xff000000u : c == '.' ? 0xffffffffu : 0;
        }
    }
    gui->cursor = SDL_CreateTexture(gui->renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STATIC, CURSOR_W, CURSOR_H);
    if (gui->cursor == NULL)
        goto texture_error;
    SDL_UpdateTexture(gui->cursor, NULL, art, CURSOR_W * 4);
    SDL_SetTextureBlendMode(gui->cursor, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(gui->cursor, SDL_SCALEMODE_NEAREST);
    SDL_HideCursor();
    SDL_StartTextInput(gui->window);
    return gui;

texture_error:
    fprintf(stderr, "terrarium: can't create a texture: %s\n", SDL_GetError());
    gui_close(gui);
    return NULL;
}

void
gui_close(Gui *gui)
{
    if (gui == NULL)
        return;
    SDL_DestroyRenderer(gui->renderer);
    SDL_DestroyWindow(gui->window);
    SDL_free(gui);
    SDL_Quit();
}

static uint32_t
translate_key(SDL_Keycode key)
{
    switch (key) {
    case SDLK_RETURN:
        return KEY_ENTER;
    case SDLK_KP_ENTER:
        return KEY_ENTER;
    case SDLK_BACKSPACE:
        return KEY_BACKSPACE;
    case SDLK_TAB:
        return KEY_TAB;
    case SDLK_ESCAPE:
        return KEY_ESCAPE;
    case SDLK_DELETE:
        return KEY_DELETE;
    case SDLK_LEFT:
        return KEY_LEFT;
    case SDLK_RIGHT:
        return KEY_RIGHT;
    case SDLK_UP:
        return KEY_UP;
    case SDLK_DOWN:
        return KEY_DOWN;
    case SDLK_HOME:
        return KEY_HOME;
    case SDLK_END:
        return KEY_END;
    case SDLK_PAGEUP:
        return KEY_PAGE_UP;
    case SDLK_PAGEDOWN:
        return KEY_PAGE_DOWN;
    }
    if (key >= SDLK_F1 && key <= SDLK_F12)
        return KEY_F1 + (uint32_t)(key - SDLK_F1);
    if (key >= 32 && key < 127)
        return (uint32_t)key;
    return 0;
}

static uint32_t
translate_modifiers(SDL_Keymod mod)
{
    uint32_t m = 0;
    if (mod & SDL_KMOD_SHIFT)
        m |= MOD_SHIFT;
    if (mod & SDL_KMOD_CTRL)
        m |= MOD_CTRL;
    if (mod & SDL_KMOD_ALT)
        m |= MOD_ALT;
    if (mod & SDL_KMOD_GUI)
        m |= MOD_SUPER;
    return m;
}

static uint32_t
translate_buttons(SDL_MouseButtonFlags flags)
{
    uint32_t b = 0;
    if (flags & SDL_BUTTON_LMASK)
        b |= 1;
    if (flags & SDL_BUTTON_RMASK)
        b |= 2;
    if (flags & SDL_BUTTON_MMASK)
        b |= 4;
    return b;
}

static void
handle_event(Gui *gui, SDL_Event *event, InputQueue *queue)
{
    SDL_ConvertEventToRenderCoordinates(gui->renderer, event);
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        uint32_t key = translate_key(event->key.key);
        if (key != 0)
            input_push(queue, event->type == SDL_EVENT_KEY_DOWN ? INPUT_KEY_DOWN : INPUT_KEY_UP,
                       key, translate_modifiers(event->key.mod), 0);
        break;
    }
    case SDL_EVENT_TEXT_INPUT: {
        const uint8_t *s = (const uint8_t *)event->text.text, *end = s + strlen(event->text.text);
        while (s < end) {
            uint32_t c = utf8_next(&s, end);
            if (c >= 32 && c != 127 && c != 0xfffd)
                input_push(queue, INPUT_TEXT, c, 0, 0);
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        gui->mouse_x = event->motion.x;
        gui->mouse_y = event->motion.y;
        gui->mouse_inside = true;
        gui->dirty = true;
        input_push(queue, INPUT_MOUSE_MOVE, (uint32_t)(int32_t)event->motion.x,
                   (uint32_t)(int32_t)event->motion.y, translate_buttons(event->motion.state));
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        uint32_t button = event->button.button == SDL_BUTTON_LEFT    ? 1
                          : event->button.button == SDL_BUTTON_RIGHT ? 2
                                                                     : 3;
        input_push(queue,
                   event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? INPUT_MOUSE_DOWN : INPUT_MOUSE_UP,
                   (uint32_t)(int32_t)event->button.x, (uint32_t)(int32_t)event->button.y, button);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        input_push(queue, INPUT_WHEEL, (uint32_t)(int32_t)event->wheel.x,
                   (uint32_t)(int32_t)-event->wheel.y, 0);
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        gui->mouse_inside = false;
        gui->dirty = true;
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        gui->dirty = true;
        break;
    }
}

bool
gui_poll(Gui *gui, InputQueue *queue)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT)
            return false;
        handle_event(gui, &event, queue);
    }
    return true;
}

// Waits for an event without taking it, so events keep their order.
void
gui_wait(Gui *gui, int timeout_ms)
{
    SDL_WaitEventTimeout(NULL, timeout_ms);
    (void)gui;
}

// Shows the latest frame, at most 60 times a second.
void
gui_show(Gui *gui, Gpu *gpu)
{
    if (!gpu->presented && !gui->dirty)
        return;
    uint64_t now = SDL_GetTicks();
    if (now - gui->last_show < FRAME_INTERVAL_MS)
        return;
    gui->last_show = now;
    if (gpu->presented)
        SDL_UpdateTexture(gui->screen, NULL, gpu->front, (int)(gpu->width * 4));
    gpu->presented = false;
    gui->dirty = false;
    SDL_SetRenderDrawColor(gui->renderer, 0, 0, 0, 255);
    SDL_RenderClear(gui->renderer);
    SDL_RenderTexture(gui->renderer, gui->screen, NULL, NULL);
    if (gui->mouse_inside) {
        SDL_FRect at = { SDL_floorf(gui->mouse_x), SDL_floorf(gui->mouse_y), CURSOR_W, CURSOR_H };
        SDL_RenderTexture(gui->renderer, gui->cursor, NULL, &at);
    }
    SDL_RenderPresent(gui->renderer);
}

#endif
