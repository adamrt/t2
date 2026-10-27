# Building Terrarium from nothing

The order in which to build the whole system again, from an empty directory
and a C compiler. Each step names the documents that specify it and says
when it's done; a step builds only on the ones before it, and each ends
with tests that keep it working through everything after.

The order follows one rule: **build the thing that lets you test the next
thing.** The CPU comes before the assembler because the assembler's output
needs somewhere to run; the assembler before the compiler because the
compiler writes assembly; the compiler before the kernel because the kernel
is written in the language; the console before the screen because a
console can be tested from a script.

## 1. Write the contracts

Write down the instruction set and the executable format before any code:
registers, encodings, faults, the header, and how a program starts. Every
later step is checked against these.

- Read: [isa.md](isa.md) (without kernel mode, for now),
  [executable.md](executable.md).
- Done when the encodings fit in one table and each fault has a cause.

## 2. The CPU and the development monitor, in C

Write the CPU: decoding, every operation, the checks behind every fault,
and cycle counting. Around it, write the development monitor, which loads
one executable and runs it in user mode on the host, answering system calls
0–6 (exit, read, write, open, close and the arguments) with the host's
files. Give it `--trace`, which prints every instruction, and fault reports
that name the faulting address.

- Read: [isa.md](isa.md), [executable.md](executable.md).
- Done when a program written as hex words prints "ok" and exits.

## 3. The assembler, in C

The assembler is the first tool, so it's written in C. Two passes over the
source: the first finds every label, the second encodes. Write a symbol file
beside each program so traces and faults show names.

Now the CPU can be tested properly: an assembly program that exercises
every instruction (`tests/isa/`), and one small program per fault, each
checking the fault it causes (`tests/faults/`). Start the test runner here.

- Read: [assembly.md](assembly.md).
- Done when every instruction and every fault has a test.

## 4. The compiler, in C

Write the compiler in C, emitting assembly for step 3: a lexer, a parser to
a syntax tree, a checker that gives every expression a type, and a
generator. Keep the generated code plain: every value in r0 (a slice in r0
and r1), temporaries on the stack, r7 as the frame pointer. Write the
runtime beside it: `start`, and a wrapper for each system call. Then make
the compiler and assembler one tool, `tc`, which keeps the assembly text as
the boundary between them.

Grow the language a test at a time (`tests/lang/`): integers and
functions, then structs, pointers, slices and bounds checks, enums and
`match`, fixed point, function values, the manifest, and modules.

- Read: [language.md](language.md).
- Done when each feature in the spec has a test that runs on the monitor.

## 5. The core library

Write the modules every program needs, in the language: `io` (files,
buffered writing, reading lines), `mem` (arenas, copying), `text`
(comparing, numbers, UTF-8) and `os` (arguments, exit, errors).

- Read: [language.md](language.md#modules).
- Done when a program can read a file a line at a time and write a report.

## 6. Host tools, in the language

Write the tools that make Terrarium's files, as Terrarium programs running
on the monitor: `mkfont` (a font drawn as text into TFN1) and `mkdisk`
(files into a TFS1 disk with the kernel placed at its start). They are the
first real programs, and they shake out the language and library.

- Read: [filesystem.md](filesystem.md), [formats.md](formats.md),
  [devices.md](devices.md#gpu) for TFN1.
- Done when `mkdisk` makes a disk whose directories a test can list.

## 7. Kernel mode and booting, in C

Add kernel mode to the CPU: separate registers for each mode, `resume`,
`wait`, `in` and `out`, the CPU's ports, and pending interrupts. Add the
console, the clock and timer, and the disk. Add `boot`, which loads the
kernel from a disk and starts it in kernel mode, and `--headless`, which
runs the machine with the terminal as its console. Add `--date` to stop the
clock at one moment, so every run is the same.

- Read: [isa.md](isa.md#kernel-and-user-mode), [devices.md](devices.md).
- Done when a kernel that prints to the console and halts boots from a
  disk.

## 8. The kernel and the shell

Write the kernel in the language, as one loop: take interrupts, pick a
process, `resume` it, handle what it asked for. Build it up in this order:
memory regions; processes and the scheduler; system calls that restart when
they have to wait; the file system; handles, files and the console; pipes;
`spawn` and `wait`. Write its runtime, which gives the library's system
functions their kernel versions, like `io.write` going to the console.

Then the first programs: `init`, `sh` (a shell with pipes, redirection and
background jobs), and the commands `ls`, `cat`, `echo`, `mkdir`, `rm`,
`cp`, `mv`, `ps`, `kill`. Whole-system tests start here (`tests/os/`): a
session of commands typed into a fresh disk, compared with what it printed
before.

- Read: [system.md](system.md).
- Done when a session can make a directory, write a file through a pipe,
  and list it.

## 9. Permissions and memory

Make the manifest mean something: the `allow` word, inherited and checked
on every call, the recovery reserve, and refused calls reported on the
console. Slide processes together when free memory is in pieces. Report how
much memory each program used, from its highest written word, and add
`trace`.

- Read: [executable.md](executable.md#permissions),
  [system.md](system.md#tracing).
- Done when a session shows a program refused, and memory compacted.

## 10. The compiler in its own language

Port the compiler and assembler to the language, file for file, step for
step, so both make the same programs byte for byte. Add a test that builds
everything with both and compares; from now on, a change to the language is
made in both, and the test says when they drift. Put the system's source on
the disk with a script that rebuilds the compiler inside Terrarium and
checks it matches.

This comes now, not earlier, because every change to the language before
this point is made once, not twice. By now the kernel and the shell have
used the language enough to settle it.

- Read: [design.md](design.md#building-from-nothing).
- Done when the compiler rebuilds itself inside Terrarium, identically.

## 11. The screen

Add the video device, the GPU and input to the machine, and give `boot` a
window, with `--screen`, `--type`, `--drag` and `--screenshot` for testing
without one. Add windows to the kernel: frames, events, clipping, and
holding the screen until windows have painted. Write the
`gfx` and `ui` modules, then the terminal and the launcher, then the
programs: the editor, files, the monitor, paint, the image viewer, and the
rest.

- Read: [devices.md](devices.md#video), [devices.md](devices.md#gpu),
  [system.md](system.md#windows), [formats.md](formats.md#tim1-images).
- Done when the desktop comes up with a terminal, and a screenshot of each
  program looks right.

## 12. Sound

Add the sound device, and the silent one for tests, which keeps the same
time. Write the `sound` module, TSN1 and `mksound`, then `play` and the
piano.

- Read: [devices.md](devices.md#sound), [formats.md](formats.md#tsn1-sounds).
- Done when `play` plays a file `mksound` made.

## 13. The network

Add the network device over the host's sockets, then the `net` module, and
`fetch`, `serve` and `chat`.

- Read: [devices.md](devices.md#network).
- Done when `serve` in one Terrarium answers `fetch` in another, in a test.

## Throughout

- **Test from the first day**, and keep every test passing: the assembly
  tests, the language tests, the system sessions, and the agreement between
  the two compilers.
- **Make runs repeatable**: a stopped clock, a silent sound device, and a
  headless machine make every session print the same thing every time.
- **Keep the machine simple**: devices that finish their work inside `out`
  leave the kernel nothing to wait for and nothing to race.
- **Write the documents with the code**: each part's document is how the
  next one gets written.
