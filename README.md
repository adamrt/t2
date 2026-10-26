# Terrarium

A tiny computer built from scratch: a 32-bit CPU and its devices in C, and
everything else written in Terrarium's own assembly and language.

```sh
make            # the machine, plus the tools built by themselves
make test
build/terrarium build/asm.bin -o build/hello.bin examples/hello.asm
build/terrarium build/hello.bin
```

| Path         | What                                                    |
| ------------ | ------------------------------------------------------- |
| `spec/`      | The CPU, the assembly language, the executable format   |
| `src/`       | The machine (C11): CPU, memory, development monitor     |
| `system/`    | Everything that runs on Terrarium                       |
| `bootstrap/` | Seed builds of the tools, see its README                |
| `tests/`     | Instruction and fault tests                             |

`build/terrarium [--trace] [--stats] program.bin [arguments...]` runs one
program directly on the machine. With `--trace` it prints every instruction,
and `--stats` prints the cycle count at exit. If a `program.sym` file sits
next to the program (the assembler writes one with `-s`), faults and traces
show label names.
