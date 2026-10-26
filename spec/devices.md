# Devices

Devices are reached through ports with `in` and `out`, in kernel mode. Every
device finishes its work before the `out` that starts it returns, so there
is nothing to wait for.

Memory addresses given to devices are physical.

## Console

The host's terminal.

| Port   |   | Meaning                                                      |
| ------ | - | ------------------------------------------------------------ |
| `0x20` | w | write one byte                                               |
| `0x21` | r | read one byte; `0xffffffff` if none is waiting, `0xfffffffe` once input has ended |

Interrupt bit 1 is raised when input arrives or ends.

## Clock and timer

| Port   |   | Meaning                                                      |
| ------ | - | ------------------------------------------------------------ |
| `0x22` | r | microseconds since the machine started (wraps after 71 minutes) |
| `0x23` | w | raise interrupt bit 0 after this many microseconds; 0 cancels |
| `0x24` | r | the host's local time, in seconds since 1970, as if the local zone were UTC |

Interrupt bits: 0 timer, 1 console, 2 input, 3 vertical blank, 4 network.

## Disk

A block device of 4096-byte blocks.

| Port   |   | Meaning                                         |
| ------ | - | ----------------------------------------------- |
| `0x30` | w | first block                                     |
| `0x31` | w | physical memory address                         |
| `0x32` | w | number of blocks                                |
| `0x33` | w | 1 reads blocks into memory, 2 writes them       |
| `0x34` | r | 0 if the last command worked, 1 if it failed    |
| `0x35` | r | number of blocks on the disk                    |

## Video

Two framebuffers live in VRAM: the back one, which the GPU draws into, and
the front one, which is on the screen. Pixels are `0xAARRGGBB` words. The
host picks the size: the largest that fits its screen, at most 1280 pixels
wide, shown at a whole-number scale. Width 0 means there is no screen.

| Port   |   | Meaning                                                    |
| ------ | - | ---------------------------------------------------------- |
| `0x40` | r | width in pixels                                            |
| `0x41` | r | height in pixels                                           |
| `0x42` | w | copy the back buffer to the front                          |
| `0x43` | w | 1 raises interrupt bit 3 at every vertical blank, 0 stops  |

The host draws the mouse cursor itself, over the front buffer.

## GPU

The GPU runs a list of commands from memory, drawing into the back buffer.
It adds the origin to every coordinate and draws only inside the clip
rectangles, so a list can't reach the rest of the screen. Images and text
come from source memory: an address `A` in a command means physical address
`source base + A - 0x10000`, which is how a program's data addresses work.

| Port   |   | Meaning                                                    |
| ------ | - | ---------------------------------------------------------- |
| `0x48` | w | physical address of the command list                       |
| `0x49` | w | its length in words                                        |
| `0x4a` | w | source base                                                |
| `0x4b` | w | source size                                                |
| `0x4c` | w | origin x                                                   |
| `0x4d` | w | origin y                                                   |
| `0x4e` | w | physical address of the clip rectangles: x, y, w, h each   |
| `0x4f` | w | number of clip rectangles, at most 64                      |
| `0x50` | w | physical address of the font                               |
| `0x51` | w | run the list                                               |
| `0x52` | r | 0, or 1 plus the word number of the command that failed    |
| `0x53` | r | pixels drawn so far                                        |

Each command is its number followed by its arguments, all words:

| Command      | Arguments                                                     |
| ------------ | ------------------------------------------------------------- |
| 1 fill       | x, y, width, height, color                                    |
| 2 line       | x0, y0, x1, y1, color                                         |
| 3 image      | address, image width, image height, sx, sy, w, h, dx, dy, mode |
| 4 text       | x, y, color, address, length in bytes, scale                  |
| 5 triangle   | x0, y0, color0, x1, y1, color1, x2, y2, color2                |
| 6 textured   | address, image width, image height, then x, y, u, v for each corner, tint |
| 7 clip       | x, y, width, height: a further clip until the next one; a negative width ends it |

A list can visit at most 16 screens' worth of pixels: every pixel inside
the clip that a command's rectangle, text or triangle covers, and every step
along a line, counts. The command that goes over fails and the rest of the
list is skipped, so no list can hold up the machine for long.

Colors with alpha below 255 blend with what's underneath. Image mode 0
copies pixels, mode 1 blends them. A triangle mixes its corners' colors; a
textured triangle samples the image at the corner coordinates, interpolated
straight across without perspective, repeating past the image's edges, and
multiplies each pixel by the tint.

Text is UTF-8, and each character takes one glyph's width. A font starts
with five words: `TFN1`, glyph width (at most 8), glyph height, the first
character, and the number of characters, numbered as in Unicode. Each glyph
follows as one byte per row, the leftmost pixel in the top bit. Characters
that aren't in the font, and bytes that aren't UTF-8, draw as `?`.

## Input

The keyboard and mouse.

| Port   |   | Meaning                                                    |
| ------ | - | ---------------------------------------------------------- |
| `0x58` | r | number of events waiting                                   |
| `0x59` | w | copy the oldest event, four words, to this physical address |

Interrupt bit 2 is raised while events are waiting.

| Event          | Words                                          |
| -------------- | ---------------------------------------------- |
| 1 key down     | 1, key, modifiers                              |
| 2 key up       | 2, key, modifiers                              |
| 3 text         | 3, character, as a Unicode number              |
| 4 mouse move   | 4, x, y, buttons held                          |
| 5 mouse down   | 5, x, y, button                                |
| 6 mouse up     | 6, x, y, button                                |
| 7 wheel        | 7, x steps, y steps                            |

Keys are characters for the keys that type one (letters in lower case), and
8 backspace, 9 tab, 10 enter, 27 escape, 127 delete, `0x100`–`0x107` left,
right, up, down, home, end, page up, page down, and `0x110` onward F1–F12.
Modifiers are bits: 1 shift, 2 control, 4 alt, 8 command. Buttons are 1
left, 2 right, 3 middle; held buttons are bits 1, 2 and 4.

## Sound

A queue of signed 16-bit mono samples, played at 22050 a second. The queue
holds at most two seconds.

| Port   |   | Meaning                                                    |
| ------ | - | ---------------------------------------------------------- |
| `0x60` | r | samples per second                                         |
| `0x61` | w | physical address of samples to queue                       |
| `0x62` | w | their length in bytes                                      |
| `0x63` | w | queue them                                                 |
| `0x64` | r | bytes queued and not yet played                            |
| `0x65` | r | 0 if the last samples were queued, 1 if they didn't fit    |

Without a window, the machine has a silent sound device that empties its
queue at the same pace.

## Network

TCP connections, made with the host's sockets. Nothing waits except
looking up a host name. A connect returns a connection at once and finishes
in the background; until it does, a send returns 0 and a receive
`0xfffffffe`. Interrupt bit 4 is raised when a connection becomes readable,
a listener has someone waiting, a connect finishes or fails, or a
connection whose last send returned 0 has room again.

| Port   |   | Meaning                                                    |
| ------ | - | ---------------------------------------------------------- |
| `0x70` | w | physical address of a host name, for connecting            |
| `0x71` | w | its length                                                 |
| `0x72` | w | port number, for connecting or listening                   |
| `0x73` | w | which connection the next command is for                   |
| `0x74` | w | physical address of a buffer                               |
| `0x75` | w | its length                                                 |
| `0x76` | w | run a command                                              |
| `0x77` | r | the command's result                                       |

| Command   | Result                                                       |
| --------- | ------------------------------------------------------------ |
| 1 connect | a connection number                                          |
| 2 listen  | a connection number, listening on the host's own address only |
| 3 accept  | a new connection, or `0xfffffffe` if nobody is waiting yet   |
| 4 send    | bytes sent, 0 if the host's buffer is full                   |
| 5 receive | bytes received, or `0xfffffffe` if none have arrived yet     |
| 6 close   | 0                                                            |

Ports are 1 to 65535. Failures, including a connect that didn't succeed
and a connection the other side closed, return `0xffffffff`.

## Starting

`build/terrarium boot disk.img` reads the disk's first block (see
[filesystem.md](filesystem.md)) to find the kernel, an ordinary executable.
The kernel's data, heap and stack go at the start of RAM, with its code
right after. Its data space covers all of RAM, so its data address `A` is
physical address `A - 0x10000`. It starts in kernel mode with:

| Register | Value                                         |
| -------- | --------------------------------------------- |
| r0, r1   | heap start and end, as for any program        |
| r2       | first free physical address, a multiple of 4096 |
| r3       | the size of RAM                               |
