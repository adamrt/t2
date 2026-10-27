# System Calls

A program asks the kernel for things with `sys N`. Arguments go in r0–r6
and the result comes back in r0; every other register keeps its value.
Failures return `-1` (`0xffffffff`). Calls 0–6 also work on the development
monitor, so the same tools run on both. Programs reach them through the
library's modules, which `/lib/runtime.asm` connects to these numbers: `io.read`,
`os.spawn`, `gfx.window_open`, `sound.play`, `net.connect` and so on.

| N  | Call       | Arguments                                         | Result                     |
| -- | ---------- | ------------------------------------------------- | -------------------------- |
| 0  | exit       | r0 status                                         |                            |
| 1  | read       | r0 handle, r1 buffer, r2 length                   | bytes read, 0 at the end   |
| 2  | write      | r0 handle, r1 bytes, r2 length                    | bytes written              |
| 3  | open       | r0 path, r1 length, r2 mode                       | handle                     |
| 4  | close      | r0 handle                                         | 0                          |
| 5  | args       |                                                   | number of arguments        |
| 6  | arg        | r0 index, r1 buffer, r2 length                    | the argument's length      |
| 7  | spawn      | r0 path, r1 length, r2 args, r3 length, r4 input, r5 output, r6 errors | process id |
| 8  | wait       | r0 process id                                     | its exit status            |
| 9  | pipe       |                                                   | reading handle; writing handle in r1 |
| 10 | info       | r0 path, r1 length, r2 buffer for kind, size and time | 0                      |
| 11 | list       | r0 path, r1 length, r2 buffer, r3 length          | bytes of records           |
| 12 | remove     | r0 path, r1 length                                | 0                          |
| 13 | make_dir   | r0 path, r1 length                                | 0                          |
| 14 | set_dir    | r0 path, r1 length                                | 0                          |
| 15 | get_dir    | r0 buffer, r1 length                              | the path's length          |
| 16 | time       |                                                   | milliseconds since start   |
| 17 | sleep      | r0 milliseconds                                   | 0                          |
| 18 | processes  | r0 buffer, r1 length                              | bytes of records           |
| 19 | kill       | r0 process id                                     | 0                          |
| 20 | memory     |                                                   | bytes for programs; free bytes in r1 |
| 21 | window_open | r0 title, r1 length, r2 width, r3 height         | handle                     |
| 22 | window_draw | r0 handle, r1 commands, r2 words                 | 0, or the GPU's error      |
| 23 | event_wait | r0 buffer for an event, r1 milliseconds (-1 forever) | 1 with an event, 0 if none came |
| 24 | available  | r0 handle                                         | bytes ready to read, -1 when nothing more will come |
| 25 | screen_info | r0 buffer for four words                         | 0                          |
| 26 | seek       | r0 handle, r1 position                            | the new position, at most the file's size |
| 27 | trace_spawns | r0 the writing end of a pipe, or -1 to stop     | 0                          |
| 28 | audio_play | r0 samples, r1 bytes                              | 0, or -1 if the queue is full |
| 29 | audio_status |                                                 | bytes waiting to play; samples per second in r1 |
| 30 | connect    | r0 host, r1 length, r2 port                       | handle                     |
| 31 | listen     | r0 port                                           | handle                     |
| 32 | accept     | r0 listening handle                               | handle for the next connection |
| 33 | clipboard_set | r0 text, r1 length (at most 64 KiB)            | 0                          |
| 34 | clipboard_get | r0 buffer, r1 length                           | the clipboard's length     |
| 35 | window_resizable | r0 handle, r1 least width, r2 least height  | 0                          |
| 36 | date       |                                                   | local time, in seconds since 1970 |
| 37 | rename     | r0 path, r1 length, r2 new path, r3 length        | 0                          |
| 38 | window_title | r0 handle, r1 title, r2 length (up to 32 bytes kept) | 0                      |

## Handles

A handle is a small number naming something a process can read or write:
the console, a file, one end of a pipe, a network connection, or a window. Every process starts with
handles 0, 1 and 2 for its input, output and errors, chosen by whoever
spawned it.

`open` modes are 0 to read, 1 to create or empty a file and write it, and
2 to write at its end. Reading a pipe or the console waits until there is
something to read; writing a full pipe waits until there is room.

`connect` returns a handle before the connection is made. Writing to it
waits until it is, and fails if connecting did. Reading a connection waits
for data, and reads 0 bytes once it has closed or failed. To check without
waiting, `available` gives the bytes a connection has waiting (-1 once it
has closed), and for a listening handle 1 when someone is waiting to be
accepted.

## Paths

Paths are separated by `/`. A path that doesn't start with `/` is relative
to the process's directory, which it gets from its parent and changes with
`set_dir`. `.` and `..` mean what you'd expect.

`rename` moves a file or directory to a new path, which may be in another
directory, without copying it. It never replaces anything already at the new
path, won't move a file that's open or a directory into itself, and leaves
a process whose directory moved with the old path.

## Programs

`spawn` starts an executable with its own memory, which it never shares.
Its arguments are the bytes in `args`, each ending with a zero byte. The new
process gets the parent's directory and the three given handles. A parent
collects an ended child's status with `wait`.

## Records

`list` fills its buffer with 64-byte records: a name of up to 48 bytes
padded with zeros, then the kind (1 file, 2 directory), the size, the time
it last changed (as from `date`), and 4 zero bytes.

`processes` fills its buffer with 64-byte records: the id, the parent's id,
the state (1 ready, 2 waiting, 3 ended), the memory reserved, the CPU cycles
used, the most stack it has used (its high-water mark), the bytes it has
read and written, and a name of up to 32 bytes.

## Windows

Windows share the screen's back buffer. A program draws in its window by
handing `window_draw` a list of GPU commands (see [devices.md](devices.md)),
with coordinates measured from the window's top left. The kernel points the
GPU at the window and clips to the parts of it that other windows don't
cover, and images and text come from the program's own memory. Windows keep
no pixels, so whenever part of one needs painting again its program gets a
draw event.

`event_wait` fills eight words: the kind, the window's handle, x, y, key,
modifiers, buttons, and 0. The kinds are 1 draw, 2 key down, 3 key up,
4 text (the key is a character's Unicode number), 5 mouse move, 6 mouse down, 7 mouse up,
8 wheel, 9 close (the close box was clicked), and 10 resize (x and y are
the new width and height). Mouse positions are
relative to the window; keys are as in [devices.md](devices.md).

`screen_info` gives the screen's width and height, and the system font's
glyph width and height. It fails when there is no screen.

The clipboard holds one piece of text that any program with the `window`
permission can set or read, for copying between programs.

The kernel draws each window's frame and title bar. Dragging the title bar
moves the window, clicking a window brings it to the front, and keys go to
the front window. After `window_resizable`, a window has a strip under it,
and dragging the strip changes its size, from the least size given (but no
more than the screen) up to the screen's size. A program only gets the
latest size if it falls behind.

## Tracing

After `trace_spawns`, every program the process starts, and every program
those start, report their system calls into the pipe as lines of text:
the program's name, the call with its path or first argument, and the
result. Reports are dropped when the pipe is full rather than slowing the
program down. `trace program arguments...` shows them.
