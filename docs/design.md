# Design

Terrarium is a computer small enough to understand completely, from its
first instruction to its desktop. The machine is a virtual one, written in
C: a CPU, a GPU, and devices for the console, time, disk, input, sound and
network. Everything that runs on it is Terrarium's own: an instruction set,
an assembly language, a programming language and its compiler, a kernel, a
window system, a shell, programs, and file formats.

This document explains why each part is the way it is. The other documents
here specify the parts: [isa.md](isa.md), [devices.md](devices.md),
[executable.md](executable.md), [assembly.md](assembly.md),
[language.md](language.md), [system.md](system.md),
[filesystem.md](filesystem.md) and [formats.md](formats.md).
[building.md](building.md) lays out the order in which to build the whole
system from nothing.

## Principles

- **Small enough to read.** Each part is short enough to read in a sitting,
  and its document is enough to write it again.
- **Its own, not borrowed.** Nothing is there for compatibility with another
  system. Where an idea comes from elsewhere, like QOI's pixel operations
  or IMA ADPCM's sound codes, the document says so, and the format around it
  is still Terrarium's.
- **One way to do each thing.** One pixel format, one executable format, one
  calling convention, one kind of event.
- **Limits are stated.** Tables have fixed sizes, programs declare their
  memory, and a program can do only what its manifest says.
- **A C99 compiler is the only dependency.** The machine, and the compiler
  that builds everything else, need nothing more.

## The machine

**The CPU** has eight registers and a stack pointer, and 45 operations, each
one 32-bit word. A fixed width means decoding is a few shifts and masks, the
pc always moves by 4, and a branch's reach is easy to state. Every
operation costs one cycle, except `copy` and `fill`, whose cost grows with
the bytes they move, so cycle counts mean something.

**Code and data are separate address spaces**, both starting at `0x10000`.
Code can only be executed: it can't be read or written, so a program can't
change itself, and constants live in data. Nothing exists below `0x10000` in
either space, so a null pointer, or a small offset from one, faults instead
of reading something.

**Fixed point instead of floating point.** `fmul` and `fdiv` work on 16.16
numbers. They need no new registers or rounding modes, give the same answer
everywhere, and are enough for the 3D, fractals and audio Terrarium does.

**The stack has a hardware limit**, so running out of stack is a fault at
the instruction that did it, not a corruption found later.

**Devices finish inside the `out` that starts them.** A disk read, a GPU
list or a network send is complete when the instruction returns. There is
no DMA to race and no completion to wait for; interrupts exist only for
things from outside: the timer, input, the console, the network and the
vertical blank.

**The machine is an interpreter.** It runs about 200 million instructions a
second on a current laptop, which is plenty, and it can count cycles, trace
every instruction, and check every access.

## Kernel mode

User and kernel mode each have their own registers, pc and address space.
The kernel runs a program with `resume`, which works like a call: the
program runs until it makes a system call, faults or is interrupted, and
then the kernel carries on at the next instruction, its own registers
untouched. A port says why the program stopped, and ports give the kernel
the program's registers. So the kernel is an ordinary loop: there are no
trap vectors, no saved interrupt frames, and interrupts never stop the
kernel itself; they wait as pending bits until it looks.

**System calls restart.** A call that has to wait rewinds the program to its
`sys` instruction and blocks it. Whenever something changes, blocked
programs run again and repeat the call. The kernel never holds a half-done
call, at the cost of retrying calls that still have to wait.

## Memory

A program declares all the memory it will ever use in its manifest. The
kernel reserves exactly that, in one unbroken run of physical memory, or
refuses to start it; `main` receives the whole heap as a slice, and the
program divides it into arenas. Nothing is allocated behind a program's
back, so it can't run out halfway through.

The CPU checks every access against a base and a size for code and for data.
That is all the memory protection needs: no page tables, no paging, no
swapping. When free memory is in pieces, the kernel slides programs together
to make room. Since memory starts zeroed, the highest word a program wrote
shows how much of its heap it used, and the lowest how much stack; `trace`
reports both, so a manifest can ask for what a program really needs.

## Programs and permissions

An executable's header is its manifest: entry point, sizes, memory, and an
`allow` word with permissions to read, write, spawn programs, open windows,
manage processes, use the recovery reserve, play audio and use the network.
Anyone can read what a program will be able to do before running it. A
program gets what it asks for, but never more than the program that started
it, and a refused call is reported on the console. 2 MiB of memory is kept
for programs with `recovery`, so `ps`, `kill` and `monitor` start even when
memory is full, and `trace` shows every system call a program makes.

## The language

The Terrarium language is small and in the spirit of C, with the rough edges
taken off.

- **Explicit types, no silent conversions** apart from widening integers
  and an array becoming a slice. Number literals take the type their use
  needs, and it's an error if they don't fit.
- **Slices are checked.** Indexing and slicing stop the program when they
  go out of bounds. `p[0:n]`, making a slice from a pointer, is the one
  unchecked operation, and it's easy to find.
- **Arenas, not a heap.** `new(arena, T)` takes zeroed memory from an arena,
  and an arena is freed all at once. With all memory handed to `main`, there
  is no `free` to forget and nothing to leak.
- **Statements end at the end of a line** when its last token could end one,
  so there are no semicolons, and a long expression breaks after an
  operator. Declarations read `var x u32` and `fn f(a u32) u32`, without
  punctuation between names and types.
- **A module is one file in the library**, imported by name. Its names are
  qualified (`io.print`), so modules never clash with each other or with a
  program, and every name says where it comes from.
- **Only what `main` reaches is compiled**, so importing a module costs
  nothing in itself.
- **No generics and no methods.** A struct can hold functions. Leaving them
  out keeps the compiler small enough to read.

## One tool: `tc`

`tc` compiles and assembles. It compiles a program's `.tr` files and the
modules they import into assembly text, then assembles that, after the
runtime and any `.asm` files it was given, into an executable. The assembler
is the library module `asm`, so its names can't mix with the compiler's,
and any program can use it.

**Assembly text stays between the two halves.** The compiler never encodes
an instruction; it writes the same assembly a person would, which `tc -S`
shows. The compiler's output can always be read, and the assembler is a
complete tool for programs written by hand.

**Functions without bodies are written in assembly**, in a runtime: system
call wrappers, `mem.move` with the `copy` instruction, and the program's
startup. A name `write` in the module `io` is `_io.write` in assembly; the
underscore keeps compiled names apart from registers, keywords and the
runtime's own labels. Programs get `/lib/runtime.asm`, where `io.write`
makes a system call. The kernel is built with `kruntime.asm` instead, where
the same function writes to the console, so library code like `io.Writer`
works in the kernel unchanged.

## Building from nothing

`bootstrap/` holds the compiler and assembler again, in C99, ported file for
file and step for step from the Terrarium sources. `make` builds the C
compiler, uses it to build the Terrarium one, and has that build itself. The
test suite builds every program with both compilers and requires the
results to be identical byte for byte, so the two can't drift apart, and the
compiler Terrarium runs holds nothing that isn't in its source. Inside
Terrarium, `sh /src/scripts/build-tc` rebuilds the compiler from the source
on the disk and checks it matches.

## Graphics and windows

**One pixel format.** A pixel is a 32-bit word, `0xAARRGGBB`, everywhere: in
the framebuffer, in GPU colors, in images and textures, and in TIM1 files
once decoded. With alpha in the top byte, an opaque color reads as `0xff`
followed by the familiar hex color, and the layout is the one hosts display
natively, so showing a frame is a copy. Alpha below 255 blends.

**The GPU draws from command lists**: fills, lines, images, text, and flat,
shaded or textured triangles. It clips every list to a set of rectangles,
and it stops a list that has touched more than 16 screens' worth of pixels,
so no program can hold up the machine.

**Windows keep no pixels.** There is one framebuffer, and every window
draws straight into it, through the GPU, which moves the drawing to the
window's place and clips it to the parts of the window no other window
covers. When part of a window comes into view, its program gets a draw event
and paints it again. Windows cost no memory, and nothing is copied. Since a
program only ever says "draw these commands in my window," the window system
could keep a picture of each window later without changing any program.

**Nothing flickers.** After uncovering part of the screen, the kernel asks
the windows there to repaint, and doesn't show the new frame until they have
(or 100 ms have passed, so a stuck program can't freeze the screen).
Dragging a window repaints only what it leaves and covers, and moves at the
pace the programs repaint.

**The window system is in the kernel.** In a user-space desktop the kernel
would still have to give one program the screen and input and carry
everything between it and the others; for a first system, putting windows
in the kernel was much smaller. Windows, events and permissions are
already shaped so that a desktop program could take over.

**Controls are drawn the immediate way.** A program redraws its window by
calling `ui.button`, `ui.checkbox` and the rest on every event, and each
says what happened to it. There is no tree of widget objects to keep in step
with the program: the program's own variables are the only state.

**Text is UTF-8**, left to right, one character per cell. Terra, the system
font, is 8 by 16 pixels, drawn as text in `system/fonts/terra.txt`, and
covers ASCII and Latin-1.

## Sound and networking

**Sound is a queue of samples**: signed 16-bit, mono, 22050 a second. A
program queues samples and the device plays them; there are no mixers or
formats to choose. TSN1 files store sounds as IMA ADPCM codes, four bits a
sample, in blocks that decode on their own.

**Networking is TCP**, through the host's sockets. The device never waits,
except for looking up a host name: connects finish in the background, and
sends and receives say when they can't go ahead yet. Listening is on the
host's own address only.

## Files and formats

Every format is Terrarium's own and can be read a piece at a time, without
holding a whole file in memory.

- **TFS1 disks** are 4 KiB blocks linked by a table, like FAT. The kernel's
  blocks are kept in one run, so the machine can load it without knowing
  the file system.
- **TRX2 executables** are a header and the program's code and data.
- **TFN1 fonts** are bitmaps, a byte per row.
- **TIM1 images** are 64×64 tiles, each compressed on its own with QOI's
  operations, so a viewer decodes only the tiles it shows.
- **TSN1 sounds** are blocks of IMA ADPCM that each decode on their own, so
  a player needs only one block at a time.

## System calls

The first seven calls (exit, read, write, open, close, and the arguments)
look like Unix's, and the development monitor answers them too, so the same
tools run on the host and inside Terrarium. The rest are Terrarium's own:
`spawn` starts a program from a file instead of copying the caller, windows
are handles that deliver events, and every call is checked against the
caller's permissions.

## Limits

- Fixed tables: 32 processes, 16 handles each and 128 open objects across
  the system, 16 pipes of 4 KiB, 64 open files, 16 windows, 16 network
  connections.
- Disks are at most 64 MiB, and nothing writes past a file's end except
  appending.
- Looking up a host name for `connect` stops the machine until the host's
  resolver answers.
- When anything changes, every blocked process runs again to retry its
  call: simple, but slow with many waiting processes.
- Text has no shaping and no right-to-left; characters outside Latin-1 show
  as `?`.
- A module is a single file, and all of its names can be used outside it.
- The test suite runs the system headless with the silent sound device.
  Graphical programs are checked with screenshots (`--screen`, `--type`,
  `--drag`, `--screenshot`), but not automatically.

## Not yet built

- **A desktop program** in user space, taking windows over from the
  kernel, and with it a picture kept for each window, so moving windows
  needs no repainting.
- **Time** as a wall clock of days and milliseconds and a monotonic clock
  apart from it; see [time.md](time.md).
- **Native translation** of hot code to ARM64.
- **UDP**, and a networked game.
- **A file dialog**, and resizing for `paint`.
