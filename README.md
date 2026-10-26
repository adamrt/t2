# Terrarium

A tiny computer built from scratch: a 32-bit CPU and its devices in C, and
everything else written in Terrarium's own assembly and language: the
assembler, the compiler, the kernel, the shell and the programs.

```sh
make            # the machine, the tools (built by themselves), and a disk
make run        # boot Terrarium: a window with a terminal, and a console here
make test
```

`make run` opens a window with the desktop and a terminal, and the host
terminal becomes Terrarium's console, with its own shell. In the terminal,
try `edit /src/hello.tr`, `cube`, `fractal`, `monitor` or `demo`.

`build/mkimage.bin` turns BMP pictures into Terrarium's TIM1 images, which
`view` shows:

```sh
build/terrarium build/mkimage.bin -o picture.tim picture.bmp
```

Inside Terrarium:

```
/> ls /bin
/> cd /src
/src> tc -o hello.asm /lib/core.tr hello.tr
/src> asm -o hello /lib/rt.asm hello.asm
/src> ./hello
```

On the host, the development monitor runs a single program with the host's
files, which is how the tools build Terrarium:

```sh
build/terrarium build/tc.bin -o build/hello.asm system/lib/core.tr examples/hello.tr
build/terrarium build/asm.bin -o build/hello.bin system/lib/rt.asm build/hello.asm
build/terrarium build/hello.bin
```

| Path         | What                                                    |
| ------------ | ------------------------------------------------------- |
| `spec/`            | The CPU, devices, assembly, the language, files, formats, system calls |
| `src/`             | The machine (C11): CPU, devices, monitor               |
| `system/asm/`      | The assembler, in assembly                             |
| `system/tc/`       | The compiler, in the language                          |
| `system/kernel/`   | The kernel                                             |
| `system/bin/`      | The shell and programs                                 |
| `system/lib/`      | The runtime and libraries                              |
| `system/tools/`    | `mkdisk`, `mkfont` and `mkimage`, which run on the host |
| `system/fonts/`    | Terra, the system font, drawn as text                  |
| `bootstrap/`       | Seed builds of the tools, see its README               |
| `tests/`           | Instruction, fault, language and system tests          |

`build/terrarium boot [--headless] [--fullscreen] disk.img [kernel.sym]`
starts the whole machine; `--headless` leaves out the screen. For testing,
`--screen WxH` gives it a screen without a window, `--type text` presses
keys once it has started, `--run-for ms` stops it, and `--screenshot
out.png` saves the screen when it stops.
`build/terrarium [--trace] [--stats] program.bin [arguments...]` runs one
program directly on the development monitor. With `--trace` it prints every instruction,
and `--stats` prints the cycle count at exit. If a `program.sym` file sits
next to the program (the assembler writes one with `-s`), faults and traces
show label names.
