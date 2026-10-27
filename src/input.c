#include "input.h"
#include "utf8.h"

#include <stddef.h>
#include <string.h>

#define TYPE_START_US 500000
#define TYPE_INTERVAL_US 20000
#define DRAG_START_US 300000
#define DRAG_INTERVAL_US 30000
#define DRAG_STEPS 10

void
input_push(InputQueue *queue, uint32_t type, uint32_t a, uint32_t b, uint32_t c)
{
    if (queue->count == INPUT_CAPACITY)
        return;
    uint32_t index = (queue->start + queue->count) % INPUT_CAPACITY;
    queue->events[index] = (InputEvent){ type, a, b, c };
    queue->count++;
}

InputEvent
input_pop(InputQueue *queue)
{
    if (queue->count == 0)
        return (InputEvent){ 0 };
    InputEvent event = queue->events[queue->start];
    queue->start = (queue->start + 1) % INPUT_CAPACITY;
    queue->count--;
    return event;
}

void
input_script_init(InputScript *script, const char *text, const int32_t *drag, uint64_t now_us)
{
    memset(script, 0, sizeof(*script));
    script->text = (const uint8_t *)(text != NULL ? text : "");
    script->text_end = script->text + strlen((const char *)script->text);
    script->drag_step = drag != NULL ? 0 : -1;
    if (drag != NULL)
        memcpy(script->drag, drag, sizeof(script->drag));
    if (script->text < script->text_end)
        script->next_event_us = now_us + TYPE_START_US;
    else if (drag != NULL)
        script->next_event_us = now_us + TYPE_START_US + DRAG_START_US;
}

// Match the window's key down, text, key up sequence for printable ASCII.
// Control characters represent special keys or Ctrl with a letter.
static void
type_character(InputQueue *queue, uint32_t character)
{
    if (character == '\n' || character == KEY_ESCAPE || character == KEY_TAB ||
        character == KEY_BACKSPACE) {
        input_push(queue, INPUT_KEY_DOWN, character == '\n' ? KEY_ENTER : character, 0, 0);
    } else if (character < 32) {
        input_push(queue, INPUT_KEY_DOWN, 'a' + character - 1, MOD_CTRL, 0);
    } else if (character < 127) {
        bool shifted = character >= 'A' && character <= 'Z';
        uint32_t key = shifted ? character - 'A' + 'a' : character;
        uint32_t modifiers = shifted ? MOD_SHIFT : 0;
        input_push(queue, INPUT_KEY_DOWN, key, modifiers, 0);
        input_push(queue, INPUT_TEXT, character, 0, 0);
        input_push(queue, INPUT_KEY_UP, key, modifiers, 0);
    } else {
        input_push(queue, INPUT_TEXT, character, 0, 0);
    }
}

static void
drag_mouse(InputScript *script, InputQueue *queue)
{
    // Widen before subtracting so even extreme command-line coordinates fit.
    const int32_t *drag = script->drag;
    int32_t x = (int32_t)(drag[0] + ((int64_t)drag[2] - drag[0]) * script->drag_step / DRAG_STEPS);
    int32_t y = (int32_t)(drag[1] + ((int64_t)drag[3] - drag[1]) * script->drag_step / DRAG_STEPS);
    if (script->drag_step == 0)
        input_push(queue, INPUT_MOUSE_DOWN, (uint32_t)x, (uint32_t)y, 1);
    input_push(queue, INPUT_MOUSE_MOVE, (uint32_t)x, (uint32_t)y, 1);
    if (script->drag_step == DRAG_STEPS) {
        input_push(queue, INPUT_MOUSE_UP, (uint32_t)x, (uint32_t)y, 1);
        script->drag_step = -1;
    } else {
        script->drag_step++;
    }
}

void
input_script_poll(InputScript *script, InputQueue *queue, uint64_t now_us)
{
    if (script->next_event_us == 0 || now_us < script->next_event_us ||
        queue->count + 3 > INPUT_CAPACITY)
        return;
    if (script->text < script->text_end) {
        type_character(queue, utf8_next(&script->text, script->text_end));
        script->next_event_us = now_us + TYPE_INTERVAL_US;
        if (script->text == script->text_end)
            script->next_event_us =
                script->drag_step >= 0 ? script->next_event_us + DRAG_START_US : 0;
    } else if (script->drag_step >= 0) {
        drag_mouse(script, queue);
        script->next_event_us = script->drag_step >= 0 ? now_us + DRAG_INTERVAL_US : 0;
    }
}
