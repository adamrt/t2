// Firmware loading and the full machine's run loop.
#include "boot.h"
#include "bytes.h"
#include "devices.h"
#include "gui.h"
#include "host.h"
#include "screenshot.h"

#include <string.h>

#define CPU_STEPS_PER_POLL 100000
#define WINDOW_POLL_MS 50
#define CONSOLE_POLL_MS 10

// Loads the kernel named by the disk's header. The kernel's data, heap and
// stack go at the start of RAM and its code right after; the kernel's data
// space covers all of RAM, so data address A is physical address A - 0x10000.
static bool
load_kernel(Machine *machine, DiskDevice *disk)
{
    if (disk->blocks == 0 || memcmp(disk->bytes, "TFS1", 4) != 0) {
        fprintf(stderr, "terrarium: not a Terrarium disk\n");
        return false;
    }
    uint32_t block = read_u32_le(disk->bytes + 28);
    uint32_t size = read_u32_le(disk->bytes + 32);
    if (block >= disk->blocks || size > (uint64_t)(disk->blocks - block) * DISK_BLOCK_SIZE) {
        fprintf(stderr, "terrarium: the disk's kernel is out of range\n");
        return false;
    }
    Executable exe;
    const char *error = parse_executable(disk->bytes + (size_t)block * DISK_BLOCK_SIZE, size, &exe);
    if (error != NULL) {
        fprintf(stderr, "terrarium: kernel: %s\n", error);
        return false;
    }
    uint32_t kernel_data_size = exe.memory_size - exe.code_size;
    memcpy(machine->ram, exe.data_bytes, exe.data_size);
    memcpy(machine->ram + kernel_data_size, exe.code_bytes, exe.code_size);

    machine->space.code = (Segment){ .base = kernel_data_size, .size = exe.code_size };
    machine->space.data = (Segment){ .base = 0, .size = RAM_SIZE };
    machine->space.stack_limit = NULL_GUARD + kernel_data_size - exe.stack_size;
    machine->pc = exe.entry_pc;
    machine->r[REG_SP] = NULL_GUARD + kernel_data_size;
    machine->r[0] = NULL_GUARD + align4(exe.data_size) + align4(exe.bss_size);
    machine->r[1] = machine->space.stack_limit;
    machine->r[2] = (kernel_data_size + exe.code_size + DISK_BLOCK_SIZE - 1) / DISK_BLOCK_SIZE *
                    DISK_BLOCK_SIZE;
    machine->r[3] = RAM_SIZE;
    machine->kernel = true;
    machine->has_kernel = true;
    return true;
}

// Include host work as well as device interrupts when the CPU is waiting.
static int
idle_timeout(const Devices *devices, const InputScript *script, const Gui *gui, uint64_t stop_at_us)
{
    uint64_t now_us = host_monotonic_us();
    int timeout_ms = devices_timeout(devices, now_us);
    timeout_ms = host_min_timeout(timeout_ms, host_deadline_ms(stop_at_us, now_us));
    timeout_ms = host_min_timeout(timeout_ms, host_deadline_ms(script->next_event_us, now_us));
    if (gui != NULL)
        timeout_ms = host_min_timeout(timeout_ms,
                                      devices->console.closed ? WINDOW_POLL_MS : CONSOLE_POLL_MS);
    return timeout_ms;
}

static int
run_machine(Machine *machine, Devices *devices, Gui *gui, const BootOptions *options,
            const SymbolTable *symbols)
{
    InputScript script;
    input_script_init(&script, options->typed_text, options->has_drag ? options->drag : NULL,
                      devices->started_at_us);
    uint64_t stop_at_us = options->run_for_ms == 0
                              ? 0
                              : devices->started_at_us + (uint64_t)options->run_for_ms * 1000;

    for (;;) {
        uint64_t now_us = host_monotonic_us();
        if (stop_at_us != 0 && now_us >= stop_at_us)
            return 0;
        input_script_poll(&script, &devices->input, now_us);
        if (gui != NULL && !gui_poll(gui, &devices->input))
            return 0;
        devices_poll(devices, machine);
        Stop stop = machine_run(machine, CPU_STEPS_PER_POLL);
        if (gui != NULL)
            gui_show(gui, &devices->gpu);

        switch (stop) {
        case STOP_BUDGET:
            break;
        case STOP_WAIT: {
            int timeout_ms = idle_timeout(devices, &script, gui, stop_at_us);
            if (devices->console.closed && timeout_ms < 0) {
                fprintf(stderr, "terrarium: idle with no input left, stopping\n");
                return 0;
            }
            if (gui != NULL)
                gui_wait(gui, timeout_ms);
            else
                devices_wait(devices, machine, timeout_ms);
            break;
        }
        case STOP_HALT:
            return 0;
        case STOP_FAULT:
            fprintf(stderr, "kernel ");
            report_fault(machine, symbols);
            return 70;
        case STOP_SYS:
            fprintf(stderr, "terrarium: unexpected system call outside the kernel\n");
            return 70;
        }
    }
}

int
boot(const BootOptions *options)
{
    Machine machine;
    if (!machine_init(&machine)) {
        fprintf(stderr, "terrarium: out of memory\n");
        return 2;
    }
    Devices devices;
    devices_init(&devices, &machine);
    SymbolTable symbols = { 0 };
    Gui *gui = NULL;
    int status = 2;
    uint32_t width = options->screen_width, height = options->screen_height;
    if (options->window) {
        gui = gui_open(options->fullscreen, &width, &height);
        if (gui == NULL)
            goto cleanup;
    }
    if (!devices_open(&devices, options->disk_path, width, height,
                      !options->window || options->mute, options->fixed_date))
        goto cleanup;
    if (!load_kernel(&machine, &devices.disk))
        goto cleanup;
    if (options->symbol_path != NULL)
        symbols_load(&symbols, options->symbol_path);

    status = run_machine(&machine, &devices, gui, options, &symbols);
    fflush(stdout);
    if (options->screenshot_path != NULL &&
        !screenshot_write(&devices.gpu, options->screenshot_path)) {
        fprintf(stderr, "terrarium: can't save screenshot %s\n", options->screenshot_path);
        if (status == 0)
            status = 2;
    }

cleanup:
    symbols_free(&symbols);
    devices_close(&devices);
    gui_close(gui);
    machine_free(&machine);
    return status;
}
