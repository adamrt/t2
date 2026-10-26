; Runtime for compiled programs: startup, system calls, and the few
; operations the language needs from assembly. Functions follow the calling
; convention in spec/language.md: arguments start at [sp + 4].

start:
    r1 -= r0                    ; main(memory: []u8) gets the whole heap
    push r1
    push r0
    call _main
    sys 0

; fn exit(status: u32);
_exit:
    r0 = [sp + 4]
    sys 0

; fn read(file: u32, buffer: []u8) -> u32;
_read:
    r0 = [sp + 4]
    r1 = [sp + 8]
    r2 = [sp + 12]
    sys 1
    return

; fn write(file: u32, bytes: []u8) -> u32;
_write:
    r0 = [sp + 4]
    r1 = [sp + 8]
    r2 = [sp + 12]
    sys 2
    return

; fn open(path: []u8, mode: u32) -> u32;
_open:
    r0 = [sp + 4]
    r1 = [sp + 8]
    r2 = [sp + 12]
    sys 3
    return

; fn close(file: u32) -> u32;
_close:
    r0 = [sp + 4]
    sys 4
    return

; fn arg_count() -> u32;
_arg_count:
    sys 5
    return

; fn arg(index: u32, buffer: []u8) -> u32;
_arg:
    r0 = [sp + 4]
    r1 = [sp + 8]
    r2 = [sp + 12]
    sys 6
    return

; fn mem_copy(to: *u8, from: *u8, count: u32);
_mem_copy:
    r0 = [sp + 4]
    r1 = [sp + 8]
    r2 = [sp + 12]
    copy r0, r1, r2
    return

; fn mem_fill(to: *u8, value: u32, count: u32);
_mem_fill:
    r0 = [sp + 4]
    r1 = [sp + 8]
    r2 = [sp + 12]
    fill r0, r1, r2
    return

; fn stop();  Stops the program with a fault, so the monitor shows where.
_stop:
    r0 = 0
    r0 = [r0]

; Called by compiled code when an index or slice is out of range.
_bounds_failed:
    r0 = 2
    r1 = bounds_message
    r2 = 20
    sys 2
    call _stop

.data
bounds_message:
    .byte "index out of bounds\n"
