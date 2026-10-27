# Terrarium

A tiny computer built from scratch, small enough to understand from its
first instruction to its desktop. The machine — a 32-bit CPU, a GPU, sound,
disk and network devices — is C. Everything that runs on it is written in
Terrarium's own language, over a few hundred lines of its assembly: the
compiler and its assembler, the kernel, the window system, the shell and
the programs.

![The Terrarium desktop: a terminal compiling a program, a monitor, a
spinning cube and a fractal](docs/images/desktop.png)

```sh
make            # the machine, the compiler (built by itself), and a disk
make run        # boot Terrarium fullscreen; this terminal becomes its console
make test       # instructions, faults, the language, and whole-system sessions
```

It needs a C99 compiler, nothing else: even Terrarium's compiler is built
from source, by its C version in `bootstrap/`. SDL3, if it's
installed, gives the machine a window and sound. Without it (or with
`make HEADLESS=1`) everything still builds and every test passes, and the
machine runs in the terminal. Terrarium fills the screen at a
whole-number scale (Cmd-Q quits on a Mac); `make run WINDOW=1` opens a
window instead.

## What's inside

| Part                          | Written in | Lines |
| ----------------------------- | ---------- | ----- |
| The machine (`src/`)          | C99        | 3,300 |
| The compiler in C (`bootstrap/`) | C99     | 5,200 |
| The compiler (`system/tc/`)   | the language | 3,900 |
| Its assembler (`system/lib/asm.tr`) | the language | 1,100 |
| The kernel (`system/kernel/`) | the language | 3,000 |
| Other library modules (`system/lib/`) | both | 1,900 |
| 41 programs (`system/bin/`)   | the language | 6,100 |

- **The CPU** has eight registers, 45 instructions of one word each, and
  16.16 fixed-point multiply and divide instead of floating point. Each
  program gets one contiguous block of memory, with null pointers and stack
  overflows caught by the hardware.
- **The language** is small and C-like: explicit types, structs, enums,
  bounds-checked slices, `fixed`, modules, and arenas instead of a heap. A
  program declares all its memory up front, and what it's allowed to do.
- **The kernel** runs programs with preemptive multitasking, pipes, a file
  system, windows, sound and networking. It enforces each program's
  permissions, keeps memory back for recovery tools, and slides programs
  together in memory when free space is in pieces.
- **The GPU** draws PS1-style: fills, lines, text, images, and shaded or
  affine-textured triangles. Windows draw straight into the one shared
  framebuffer, clipped to the parts other windows don't cover.
- **Text is UTF-8** everywhere. Terra, the system font, is drawn as text in
  `system/fonts/terra.txt` and covers ASCII and Latin-1.

![The editor with a selection, Life, the piano and the launcher](docs/images/apps.png)

## Inside Terrarium

The desktop starts with a launcher and a terminal. Programs live in `/bin`
(`help` lists them, and `help edit` explains one), the library's modules in
`/lib`, the documentation in `/docs`, and the whole system's source in
`/src`.

```
/> cd /src
/src> tc -o hello hello.tr
/src> ./hello
Hello from the Terrarium language!
/src> sh /src/scripts/build-tc
```

`tc` compiles a program, finding the modules it imports, like `io`, in
`/lib`, and assembles it with the runtime, `/lib/runtime.asm`. `tc -S`
writes the assembly instead, and given `.asm` files, `tc` assembles them
too. A program in the current directory runs as `./name`;
without a `/`, the shell looks in `/bin`. `clear` clears the terminal.

The last line rebuilds the compiler inside Terrarium and checks that the
result is byte for byte the compiler the host built. Some others to try:

- `edit file` — a text editor (Shift or drag selects; Ctrl-S, O, A, C, X, V,
  Z, Y, F); `files` — a file browser
- `paint`, `view` — draw pixel art and look at TIM1 images
- `cube`, `fractal`, `demo` — fixed-point 3D, Mandelbrot, the GPU
- `worm`, `life`, `piano`, `calc`, `clock` — two games, an instrument, and
  two tools; the piano records (R) and `play piano.tsn` plays it back
- `monitor`, `ps`, `mem` — memory reserved and instructions used; the
  monitor can stop a process
- `manifest /bin/edit` — what a program declares about itself
- `trace ls` — every system call a program makes, and how much memory it
  used; `search text files...` — the lines that contain some text; `sort`;
  `hex file` — its bytes
- `serve 8080` — share the current directory; then `curl localhost:8080/`
  on the host. `fetch host port /path` goes the other way.
- `chat 7000` in one Terrarium and `chat localhost 7000` in another (or
  `nc localhost 7000` on the host) — talk across the network

## On the host

The tools are Terrarium programs too. On the host they run on the
development monitor, which runs one program and lends it the host's files:

```sh
build/terrarium build/tc.bin -L system/lib -o build/hello.bin examples/hello.tr
build/terrarium build/hello.bin
build/terrarium build/mkimage.bin -o picture.tim picture.bmp    # BMP to TIM1
build/terrarium build/mksound.bin -o sound.tsn recording.wav     # WAV to TSN1
```

`build/terrarium [--trace] [--stats] program.bin [arguments...]` runs a
program on the monitor. `--trace` prints every instruction, and `--stats`
the cycles it ran and how much of its heap and stack it used; a `program.sym` next to the program (`tc` writes one with
`-s`) puts label names in traces and fault reports.

`build/terrarium boot [--headless] [--fullscreen] [--mute] disk.img
[kernel.sym]` starts the whole machine. For testing, `--screen WxH` gives it
a screen without a window, `--type text` presses keys once it has started,
`--run-for ms` stops it, and `--screenshot out.png` saves the screen. In
`--type`, a newline is Enter, `\x1b` is Escape, `\b` is Backspace, and other
control characters are Ctrl with a letter. `--drag x0,y0,x1,y1` drags the mouse after the
typing, and `--date seconds` stops the clock at one moment.

The compiler is built by its C version in `bootstrap/`, then rebuilds
itself; see its README.

## Documentation

| Spec                                   |                                     |
| -------------------------------------- | ----------------------------------- |
| [isa.md](spec/isa.md)                  | the CPU, kernel mode, ports         |
| [devices.md](spec/devices.md)          | console, timer, disk, video, GPU, input, sound, network |
| [assembly.md](spec/assembly.md)        | the assembly language               |
| [language.md](spec/language.md)        | the Terrarium language              |
| [executable.md](spec/executable.md)    | programs, memory, permissions       |
| [system.md](spec/system.md)            | system calls, windows, tracing      |
| [filesystem.md](spec/filesystem.md)    | the disk                            |
| [formats.md](spec/formats.md)          | images and the other file formats   |

[docs/notes.md](docs/notes.md) explains the decisions behind the design,
where it differs from the original plan, and what it doesn't do yet.
