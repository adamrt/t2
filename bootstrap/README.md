# Bootstrap

Terrarium's compiler, assembler included, is written in the Terrarium
language, so building it needs a compiler that already exists. This is that:
the same compiler written in C99, so a C compiler is all it takes to build
Terrarium from its source.

`tc/` is a port of `system/tc/*.tr`, file for file, and of the assembler it
uses, `system/lib/asm.tr`, as `tc/asm.c`.

`make` builds it as `build/tc0`, uses it to build the Terrarium compiler from
source, then has that rebuild itself, so the compiler that runs inside
Terrarium is built by itself.

The port does what the original does, step for step, so for the same input
both make the same program byte for byte. `make test` checks that on
everything the build and the tests compile and assemble. So a change to the
language or the assembler is made in both places; the test says when they
drift apart. Having two compilers agree also shows that the compiler
Terrarium builds holds nothing that isn't in its source.
