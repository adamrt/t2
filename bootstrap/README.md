# Bootstrap

Terrarium's assembler is written in Terrarium assembly and its compiler in
the Terrarium language, so building them needs an assembler and a compiler
that already exist. These are those: the same two tools written in C99, so
a C compiler is all it takes to build Terrarium from its source.

- `asm.c` is the assembler, a port of `system/asm/asm.asm`.
- `tc/` is the compiler, a port of `system/tc/*.tr`, file for file.

`make` builds them as `build/asm0` and `build/tc0`, uses them to build the
Terrarium assembler and compiler from source, then has those rebuild
themselves, so the tools that run inside Terrarium are built by themselves.

Each port does what the original does, step for step, so for the same input
both give the same output byte for byte. `make test` checks that on
everything the build compiles and assembles. So a change to the assembler or
the language is made in both places; the test says when they drift apart.
Having two compilers agree also shows that the compiler Terrarium builds
holds nothing that isn't in its source.
