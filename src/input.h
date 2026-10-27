#pragma once

#include <stdbool.h>
#include <stdint.h>

// The four words delivered by the input device; see docs/devices.md.
enum {
    INPUT_KEY_DOWN = 1, // a: key, b: modifiers
    INPUT_KEY_UP = 2,
    INPUT_TEXT = 3,       // a: character
    INPUT_MOUSE_MOVE = 4, // a: x, b: y, c: held buttons
    INPUT_MOUSE_DOWN = 5, // a: x, b: y, c: button
    INPUT_MOUSE_UP = 6,
    INPUT_WHEEL = 7, // a: x steps, b: y steps
};

enum {
    KEY_BACKSPACE = 8,
    KEY_TAB = 9,
    KEY_ENTER = 10,
    KEY_ESCAPE = 27,
    KEY_DELETE = 127,
    KEY_LEFT = 0x100,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_F1 = 0x110,
};

enum {
    MOD_SHIFT = 1,
    MOD_CTRL = 2,
    MOD_ALT = 4,
    MOD_SUPER = 8,
};

typedef struct {
    uint32_t type, a, b, c;
} InputEvent;

#define INPUT_CAPACITY 256

typedef struct {
    InputEvent events[INPUT_CAPACITY];
    uint32_t start;
    uint32_t count;
} InputQueue;

// A full queue drops new events. Popping an empty queue returns a zero event.
void input_push(InputQueue *queue, uint32_t type, uint32_t a, uint32_t b, uint32_t c);
InputEvent input_pop(InputQueue *queue);

// Scripted keyboard and mouse input used by the host's boot options.
typedef struct {
    const uint8_t *text;
    const uint8_t *text_end;
    int32_t drag[4];
    int drag_step;          // -1 when no drag remains.
    uint64_t next_event_us; // 0 when the script has finished.
} InputScript;

void input_script_init(InputScript *script, const char *text, const int32_t *drag, uint64_t now_us);
void input_script_poll(InputScript *script, InputQueue *queue, uint64_t now_us);
