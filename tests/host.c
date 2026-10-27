// Host-side regressions that don't require a guest compiler or a window.
#include "bytes.h"
#include "gpu.h"
#include "host.h"
#include "input.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void
test_executable_sizes(void)
{
    uint8_t file[36] = { 0 };
    memcpy(file, "TRX2", 4);
    write_u32_le(file + 4, NULL_GUARD);
    write_u32_le(file + 8, 4);
    write_u32_le(file + 20, 4);
    write_u32_le(file + 24, 8);
    Executable executable;
    assert(parse_executable(file, sizeof(file), &executable) == NULL);
    assert(executable.code_bytes == file + EXECUTABLE_HEADER_SIZE);
    for (uint32_t extra = 0; extra < 3; extra++) {
        write_u32_le(file + 16, UINT32_MAX - extra);
        assert(parse_executable(file, sizeof(file), &executable) != NULL);
    }
    write_u32_le(file + 16, 1);
    assert(parse_executable(file, sizeof(file), &executable) != NULL);
    write_u32_le(file + 24, 12);
    assert(parse_executable(file, sizeof(file), &executable) == NULL);
    write_u32_le(file + 8, UINT32_MAX - 3);
    assert(parse_executable(file, sizeof(file), &executable) != NULL);
}

static void
test_screen_sizes(void)
{
    Gpu gpu;
    // Multiplying this pixel count by eight used to wrap to zero.
    assert(!gpu_init(&gpu, 0x80000000u, 0x40000000u));
    gpu_free(&gpu);
    assert(gpu_init(&gpu, 32, 24));
    assert(gpu.front == gpu.back + 32 * 24);
    gpu_free(&gpu);
}

static void
test_input_queue(void)
{
    InputQueue queue = { 0 };
    for (uint32_t i = 0; i < INPUT_CAPACITY; i++)
        input_push(&queue, INPUT_TEXT, i, 0, 0);
    input_push(&queue, INPUT_TEXT, UINT32_MAX, 0, 0);
    for (uint32_t i = 0; i < INPUT_CAPACITY; i++) {
        assert(input_pop(&queue).a == i);
        input_push(&queue, INPUT_TEXT, i + INPUT_CAPACITY, 0, 0);
    }
    for (uint32_t i = 0; i < INPUT_CAPACITY; i++)
        assert(input_pop(&queue).a == i + INPUT_CAPACITY);
    assert(input_pop(&queue).type == 0);
}

static void
test_scripted_input(void)
{
    InputScript script;
    InputQueue queue = { 0 };
    int32_t drag[4] = { 10, 20, 30, 40 };
    input_script_init(&script, "A\n\xc3\xa9", drag, 100);
    input_script_poll(&script, &queue, 500099);
    assert(queue.count == 0);
    input_script_poll(&script, &queue, 500100);
    InputEvent event = input_pop(&queue);
    assert(event.type == INPUT_KEY_DOWN && event.a == 'a' && event.b == MOD_SHIFT);
    event = input_pop(&queue);
    assert(event.type == INPUT_TEXT && event.a == 'A');
    assert(input_pop(&queue).type == INPUT_KEY_UP);
    input_script_poll(&script, &queue, 520100);
    event = input_pop(&queue);
    assert(event.type == INPUT_KEY_DOWN && event.a == KEY_ENTER);
    input_script_poll(&script, &queue, 540100);
    event = input_pop(&queue);
    assert(event.type == INPUT_TEXT && event.a == 0xe9);
    input_script_poll(&script, &queue, 860099);
    assert(queue.count == 0);
    input_script_poll(&script, &queue, 860100);
    event = input_pop(&queue);
    assert(event.type == INPUT_MOUSE_DOWN && event.a == 10 && event.b == 20);
    assert(input_pop(&queue).type == INPUT_MOUSE_MOVE);
    for (uint64_t step = 1; step <= 10; step++)
        input_script_poll(&script, &queue, 860100 + step * 30000);
    for (uint32_t step = 1; step <= 10; step++) {
        event = input_pop(&queue);
        assert(event.type == INPUT_MOUSE_MOVE && event.a == 10 + step * 2);
    }
    event = input_pop(&queue);
    assert(event.type == INPUT_MOUSE_UP && event.a == 30 && event.b == 40);
    assert(script.next_event_us == 0 && queue.count == 0);
}

static void
test_deadlines(void)
{
    assert(host_deadline_ms(0, 100) == -1);
    assert(host_deadline_ms(100, 100) == 0);
    assert(host_deadline_ms(100, 101) == 0);
    assert(host_deadline_ms(101, 100) == 1);
    assert(host_deadline_ms(1101, 100) == 2);
    assert(host_deadline_ms(UINT64_MAX, 100) == INT_MAX);
    assert(host_min_timeout(-1, 10) == 10);
    assert(host_min_timeout(10, -1) == 10);
    assert(host_min_timeout(10, 0) == 0);
}

int
main(void)
{
    test_executable_sizes();
    test_screen_sizes();
    test_input_queue();
    test_scripted_input();
    test_deadlines();
    puts("host regressions passed");
    return 0;
}
