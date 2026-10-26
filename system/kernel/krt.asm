; Kernel runtime: startup, the instructions the language can't reach, and the
; core library's system functions, which in the kernel mostly go to the
; console. Arguments start at [sp + 4], as in spec/language.md.

CONSOLE_WRITE = 0x20

start:
    r4 = _free_memory
    [r4] = r2
    r4 = _ram_size
    [r4] = r3
    r1 -= r0
    push r1
    push r0
    call _main
    halt

; fn io_read(port: u32) -> u32;
_io_read:
    r1 = [sp + 4]
    r0 = io r1
    return

; fn io_write(port: u32, value: u32);
_io_write:
    r1 = [sp + 4]
    r0 = [sp + 8]
    io r1 = r0
    return

; fn resume_user();  Runs the user program until it traps.
_resume_user:
    resume
    return

; fn wait_interrupt();
_wait_interrupt:
    wait
    return

; fn power_off();
_power_off:
    halt

; fn io.write(file: u32, bytes: []u8) -> u32;  Everything goes to the console.
_io.write:
    r1 = [sp + 8]
    r2 = [sp + 12]
    r0 = r2
    r3 = CONSOLE_WRITE
console_next:
    if r2 == 0 goto console_done
    r4 = u8 [r1]
    io r3 = r4
    r1 += 1
    r2 -= 1
    goto console_next
console_done:
    return

; The kernel has no files of its own, so these fail.
_io.read:
_io.open:
_io.close:
_os.arg:
    r0 = -1
    return

_os.arg_count:
    r0 = 0
    return

; fn os.exit(status: u32);
_os.exit:
    halt

; fn mem.move(to: *u8, from: *u8, count: u32);
_mem.move:
    r0 = [sp + 4]
    r1 = [sp + 8]
    r2 = [sp + 12]
    copy r0, r1, r2
    return

; fn mem.fill(to: *u8, value: u32, count: u32);
_mem.fill:
    r0 = [sp + 4]
    r1 = [sp + 8]
    r2 = [sp + 12]
    fill r0, r1, r2
    return

; fn os.stop();  Faults on purpose, so the machine shows where the kernel was.
_os.stop:
    r0 = 0
    r0 = [r0]

_bounds_failed:
    r0 = 28
    push r0
    r0 = bounds_message
    push r0
    r0 = 1
    push r0
    call _io.write
    call _os.stop

.data
bounds_message:
    .byte "kernel: index out of bounds\n"
