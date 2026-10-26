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

; The rest are Terrarium's own system calls (spec/system.md). The
; development monitor doesn't have them.

; fn spawn(path: []u8, args: []u8, input: u32, output: u32, errors: u32) -> u32;
_spawn:
    r0 = [sp + 4]
    r1 = [sp + 8]
    r2 = [sp + 12]
    r3 = [sp + 16]
    r4 = [sp + 20]
    r5 = [sp + 24]
    r6 = [sp + 28]
    sys 7
    return

; fn wait(process: u32) -> u32;
_wait:
    r0 = [sp + 4]
    sys 8
    return

; fn open_pipe(write_end: *u32) -> u32;
_open_pipe:
    sys 9
    r2 = [sp + 4]
    [r2] = r1
    return

; fn info(path: []u8, info: *FileInfo) -> u32;
_info:
    r0 = [sp + 4]
    r1 = [sp + 8]
    r2 = [sp + 12]
    sys 10
    return

; fn list(path: []u8, buffer: []u8) -> u32;
_list:
    r0 = [sp + 4]
    r1 = [sp + 8]
    r2 = [sp + 12]
    r3 = [sp + 16]
    sys 11
    return

; fn remove(path: []u8) -> u32;
_remove:
    r0 = [sp + 4]
    r1 = [sp + 8]
    sys 12
    return

; fn make_dir(path: []u8) -> u32;
_make_dir:
    r0 = [sp + 4]
    r1 = [sp + 8]
    sys 13
    return

; fn set_dir(path: []u8) -> u32;
_set_dir:
    r0 = [sp + 4]
    r1 = [sp + 8]
    sys 14
    return

; fn get_dir(buffer: []u8) -> u32;
_get_dir:
    r0 = [sp + 4]
    r1 = [sp + 8]
    sys 15
    return

; fn time() -> u32;
_time:
    sys 16
    return

; fn sleep(milliseconds: u32);
_sleep:
    r0 = [sp + 4]
    sys 17
    return

; fn processes(buffer: []u8) -> u32;
_processes:
    r0 = [sp + 4]
    r1 = [sp + 8]
    sys 18
    return

; fn kill(process: u32) -> u32;
_kill:
    r0 = [sp + 4]
    sys 19
    return

; fn memory_info(free: *u32) -> u32;
_memory_info:
    sys 20
    r2 = [sp + 4]
    [r2] = r1
    return

; fn window_open(title: []u8, width: u32, height: u32) -> u32;
_window_open:
    r0 = [sp + 4]
    r1 = [sp + 8]
    r2 = [sp + 12]
    r3 = [sp + 16]
    sys 21
    return

; fn window_draw(window: u32, commands: []u32) -> u32;
_window_draw:
    r0 = [sp + 4]
    r1 = [sp + 8]
    r2 = [sp + 12]
    sys 22
    return

; fn event_wait(event: *Event, timeout: u32) -> u32;
_event_wait:
    r0 = [sp + 4]
    r1 = [sp + 8]
    sys 23
    return

; fn available(handle: u32) -> u32;
_available:
    r0 = [sp + 4]
    sys 24
    return

; fn screen_info(info: *ScreenInfo) -> u32;
_screen_info:
    r0 = [sp + 4]
    sys 25
    return

; fn seek(handle: u32, position: u32) -> u32;
_seek:
    r0 = [sp + 4]
    r1 = [sp + 8]
    sys 26
    return

; fn trace_spawns(handle: u32) -> u32;
_trace_spawns:
    r0 = [sp + 4]
    sys 27
    return

; fn audio_play(samples: []u8) -> u32;
_audio_play:
    r0 = [sp + 4]
    r1 = [sp + 8]
    sys 28
    return

; fn audio_status(rate: *u32) -> u32;
_audio_status:
    sys 29
    r2 = [sp + 4]
    [r2] = r1
    return

; fn connect(host: []u8, port: u32) -> u32;
_connect:
    r0 = [sp + 4]
    r1 = [sp + 8]
    r2 = [sp + 12]
    sys 30
    return

; fn listen(port: u32) -> u32;
_listen:
    r0 = [sp + 4]
    sys 31
    return

; fn accept(listener: u32) -> u32;
_accept:
    r0 = [sp + 4]
    sys 32
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
