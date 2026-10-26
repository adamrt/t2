# Terrarium

A tiny computer built from scratch: a 32-bit CPU and its devices in C, and
everything else written in Terrarium's own assembly and language: the
assembler, the compiler, the kernel, the shell and the programs.

```sh
make            # the machine, the tools (built by themselves), and a disk
make run        # boot Terrarium in this terminal
make test
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
| `spec/`            | The CPU, devices, assembly, the language, files, system calls |
| `src/`             | The machine (C11): CPU, devices, monitor               |
| `system/asm/`      | The assembler, in assembly                             |
| `system/tc/`       | The compiler, in the language                          |
| `system/kernel/`   | The kernel                                             |
| `system/bin/`      | The shell and programs                                 |
| `system/lib/`      | The runtime and libraries                              |
| `system/tools/`    | `mkdisk`, which builds disk images                     |
| `bootstrap/`       | Seed builds of the tools, see its README               |
| `tests/`           | Instruction, fault, language and system tests          |

`build/terrarium boot disk.img [kernel.sym]` starts the whole machine.
`build/terrarium [--trace] [--stats] program.bin [arguments...]` runs one
program directly on the development monitor. With `--trace` it prints every instruction,
and `--stats` prints the cycle count at exit. If a `program.sym` file sits
next to the program (the assembler writes one with `-s`), faults and traces
show label names.
