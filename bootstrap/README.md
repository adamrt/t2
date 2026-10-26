# Seeds

Terrarium's tools are written in Terrarium's own languages, so building them
needs a previous build. These are those builds.

- `asm.bin` is the assembler, built from `system/asm/asm.asm`.

`make` uses each seed to build the tool from source, then rebuilds it with
the result, so the tools you run are always built by themselves. After
changing a tool, `make seed` checks that it reproduces itself exactly and
replaces its seed.

To use a new language feature inside a tool's own source, add the feature
first, run `make seed`, then start using it.
