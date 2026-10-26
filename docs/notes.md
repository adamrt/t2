# Notes

How Terrarium turned out, where it differs from the original plan and why,
and what it doesn't do yet.

## Decisions

**The machine.** 52 MiB of RAM and 12 MiB of VRAM. Eight registers plus sp,
and 45 one-word instructions. Code and data are separate address spaces, and
both start at 0x10000, so null pointers and small offsets from them fault.
The stack has a hardware limit. Instead of floating point there are 16.16
fixed-point `fmul` and `fdiv`. Devices finish their work inside the `out`
instruction that starts them, so there's no DMA timing to handle.

**Kernel mode.** User and kernel mode each have their own registers. The
kernel runs a process with `resume`, which acts like a call: the next
instruction runs when the process makes a system call, faults or is
interrupted. The kernel is an ordinary loop, with no trap vectors or saved
interrupt frames.

**Restartable system calls.** A call that has to wait rewinds the process's
pc to its `sys` instruction and blocks it. When something changes, blocked
processes run again and repeat the call. The kernel never keeps half-finished
calls.

**Tools in Terrarium's own languages.** The assembler is written in
Terrarium assembly and the compiler in the Terrarium language. Throwaway
Python versions (not kept) built them once; since then each builds itself
from the seeds in `bootstrap/`. Inside Terrarium,
`sh /src/scripts/build-tc` rebuilds the compiler and checks that it matches
the host's build byte for byte.

**Memory.** A program declares its memory once. The kernel reserves exactly
that or refuses to start it, and `main` gets the whole heap as a slice to
divide into arenas. Each process's memory is one unbroken run of physical
memory, which the kernel slides together when free memory is in pieces
(`tests/os/compact.txt`). There is no paging and no swapping.

**Graphics, as you suggested.** There is one shared, double-buffered
framebuffer in VRAM. Windows keep no pixels of their own. A program sends
GPU command lists in its window's coordinates; the GPU offsets them and clips
them to the parts of the window nothing covers, and reads images only from
the program's own memory. When part of a window needs painting again, its
program gets a draw event. Since programs only ever see "draw these commands
in my window," per-window buffers could replace this without changing them.
The host draws the mouse cursor.

**Permissions.** Each executable's header carries an `allow` word (read,
write, spawn, window, processes, recovery, audio, network). A program gets
what it asks for, limited to what its parent has. 2 MiB of memory is kept
back for programs with `recovery`, so `ps`, `kill` and `monitor` start even
when memory is full. `trace` shows every system call a program makes.

**Formats.** All are Terrarium's own: `TRX2` programs, the `TFS1` disk (a
FAT-like table with 4 KiB blocks), `TFN1` bitmap fonts, and `TIM1` images
(64×64 tiles, each compressed on its own with QOI's operations, so a viewer
decodes only what it shows). Terra, the system font, is drawn in
`system/fonts/terra.txt`.

## Where it differs from the plan

- **The window manager is in the kernel**, not a separate desktop program.
  Doing it in user space needs a privileged protocol between the desktop and
  the kernel; the kernel version was much smaller for a first system.
- **The interpreter is the only way programs run.** It manages about 200
  million instructions a second on an M3. There is no native translation.
- **Networking is TCP only**, using the host's sockets, and listening only
  on the host's loopback address.
- **Sound is a stream of samples.** There's no audio file format yet.
- **System calls look a little like Unix** (handles, read, write, open),
  because the tools needed to run both on the host and inside Terrarium.
  Everything else (spawn instead of fork, windows, permissions) is
  Terrarium's own.

## Limits

- Fixed tables: 32 processes, 128 open handles across the system, 16 pipes
  of 4 KiB, 16 windows, 64 open files, 16 network connections.
- Looking up a host name for `connect` stops the whole machine until the
  host's resolver answers. Connecting itself happens in the background.
- Disks are at most 64 MiB. There is no rename, and nothing writes past a
  file's end except appending.
- When anything happens, every blocked process runs again to retry its
  call. That's simple, but it isn't efficient with many waiting processes.
- Text is ASCII in one 8×16 bitmap font.
- The language has no function pointers, generics or methods, and every
  top-level name in a program shares one namespace, so library names need
  care.
- The editor copies whole lines; it has no selection.
- Sound through the host was never listened to. The tests only use the
  silent device, which keeps the same time.
- The system tests run headless. The graphical programs were checked with
  screenshots (`--screen`, `--type`, `--drag`, `--screenshot`), but not
  automatically.

## Ideas

- A user-space desktop, now that windows, events and permissions exist.
- Function pointers and modules in the language.
- Translating hot code to native ARM64.
- UDP, and a chat program or a networked game.
- A streaming audio format and a player.
- A file dialog, selection in the editor, resizing for more programs.
