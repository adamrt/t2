# Building Terrarium from nothing

This is a roadmap for writing Terrarium again, by hand, over many months.
The destination is a small computer for useful software: its own CPU,
assembly language, higher-level language, operating system and desktop.
The existing implementation demonstrates that those pieces fit together;
it is a reference, not a requirement to reproduce every implementation
choice or all 41 applications.

The order follows one rule: **build the thing that lets you test the next
thing.** Each milestone should leave something working that can be put
down and returned to. The completion checks below describe that result,
not a promise about how many days it takes.

The other documents describe the current system, except [time.md](time.md),
which is already a proposal. Recommendations here, such as buffered windows,
explicit frame completion and richer errors, are changes for the rewrite;
they are not features the current system already provides.

## Split the machine before building it

**The first machine needs only a CPU and memory.** It does not need audio,
video, keyboard or mouse input, a disk, a filesystem, or an operating system.
It does not even need to print. A test can put instruction bytes in memory,
run them, and inspect the registers and memory afterwards.

Keep these responsibilities separate as they arrive:

| Part | Responsibility | Added when |
| ---- | -------------- | ---------- |
| CPU core | Registers, instruction decoding, arithmetic, checked memory access, faults, cycle accounting; later privilege and traps | First |
| Executable loader | Validate a file and establish code, data, stack and entry registers | When assembly programs become files |
| Development monitor | Run one guest program and service a few development system calls using host streams and files | When programs need output and compiler tools need files |
| Devices | Console, timer, disk, then optional video, input, audio and network | One device at a time |
| Machine runner | Connect CPU and devices, deliver interrupts, poll the host and wait when idle | When booting a guest kernel |
| Guest software | Runtime, kernel, libraries, compiler and applications | As their prerequisites become available |

The core should expose an operation like `run(instruction_budget)` that
returns a reason: budget exhausted, halt, fault, or system call; add waiting
when kernel mode arrives. The caller decides what happens next. The core
must not open files, print diagnostics, sleep, poll SDL, or read the host
clock. Return fault details as data so a test and a debugger can use the
same result. Distinguish the instruction budget from the ISA's cycle count:
`copy` and `fill` cost more than one cycle.

Later, a small port interface connects devices to the CPU. A switch over
port numbers and an opaque device context are enough; there is no need for
a device framework, buses, hot plugging or threads. Missing devices have a
defined result. A program running without a screen is an ordinary supported
configuration.

The current separation between [machine.c](../src/machine.c),
[monitor.c](../src/monitor.c), [devices.c](../src/devices.c) and
[boot.c](../src/boot.c) is a useful reference. In the rewrite, make a CPU
test target that links only the core and its test harness. Keep the RAM
profile separate from optional VRAM and device allocations.

Keep the current 52 MiB RAM / 12 MiB VRAM profile initially if those limits
are part of the appeal. Treat them as a hardware profile rather than
scattering them through applications. Query capacities at the appropriate
interface, so a larger future profile does not require a new instruction
set. A higher limit does not remove the need for bounded allocations.

```text
CPU + memory tests
    -> assembler + executable loader
        -> development monitor
            -> small compiler + runtime + core library
                -> kernel boot -> one user program
                    -> read-only disk -> processes + console shell
                        -> writable files -> edit / build / run

After the language and monitor work: self-hosting can proceed independently.
After kernel boot works: video and input can be added independently.
After processes and blocking I/O work: audio and networking are separate branches.
```

## 1. Write the first contracts

Start with the small part of [isa.md](isa.md) needed for execution:
registers, instruction encoding, byte order, arithmetic, code and data
addresses, alignment, stack behavior, faults and stopping. Reserve the
kernel instructions without implementing them yet. Keep the current fixed
32-bit instructions and register architecture unless writing the first
tests gives a concrete reason to change them.

Sketch the executable and calling convention now, but do not finish the
window, sound or network protocols. Those are independent design tasks.
Decide whether this is an exact reimplementation or a new version: for a
personal rewrite, preserving the ideas while allowing documented ABI and
format changes is reasonable. Change version identifiers when meanings
change; an old file should be rejected rather than misread.

- Read: [isa.md](isa.md), [executable.md](executable.md), the calling
  convention in [language.md](language.md#calling-convention).
- Done when a few instruction examples have unambiguous expected states,
  and every initial fault has a defined cause and effect.

## 2. CPU and memory, in C

Start the test runner here. Have it write little-endian instruction words
directly into memory; do not depend on an assembler to test the decoder.
The first program can load 40, add 2 and halt. Its success condition is
`r0 == 42`, not text on a screen. This initial standalone execution context
can halt; once privilege is implemented, keep `halt` privileged as specified
and use `sys exit` for ordinary user programs.

Grow in small groups: constants and arithmetic; loads and stores; branches;
calls and the stack; bulk memory operations. Add signed and fixed-point
edge cases when implementing those instructions. One straightforward
decode-and-execute switch is enough.

Check complete ranges before accessing memory, including overflow in size
calculations. Define precisely when a bad branch target faults and what
state has committed by then. For an instruction that itself faults, check
that registers and memory have not partly changed. When ports arrive, this
also matters for reads that consume input.

- Done when hand-encoded programs exercise normal execution and faults,
  with exact register/memory assertions, and an infinite loop stops at the
  harness's execution budget.
- Leave out: executable files, console I/O, privilege switching, all
  devices, and host timing. Keep this test target working forever.

## 3. Executable loading and the assembler, in C

First load a tiny hand-built executable. Validate its header, entry point,
aligned sizes, memory reservation and file length before copying anything.
Loading is outside the instruction interpreter. Test truncated files and
overflowing sizes as well as valid programs.

Then write the assembler: instructions and registers, labels and branches,
data and zeroed storage, and the manifest directives. Two passes are a good
fit. Keep instruction sizing stable between passes, including pseudo
instructions. Compare a few assembled words with independently written
encodings, so an assembler and decoder cannot hide matching mistakes.

Add a disassembler, labels in fault reports and a trace option here. These
will save more time during the rewrite than early execution optimizations.

- Read: [assembly.md](assembly.md), [executable.md](executable.md).
- Done when assembly programs run and the suite includes invalid assembly,
  malformed executables, every implemented instruction and every initial
  fault. Retain the direct CPU tests from step 2.

## 4. The development monitor

Teach the runner to stop at `sys` and let the monitor answer it. Add `exit`
and writing to a captured output stream first. Then add read, open, close
and arguments as tools need them. The monitor is a small host implementation
of that ABI, not the guest kernel and not a console device.

Use the same guest runtime wrappers that will eventually call the kernel.
Specify partial reads and writes, end of stream, invalid handles, invalid
buffers and errors now. A small set of named error codes is enough; useful
software needs to distinguish end of file from failure. Keep transient
device readiness separate from both when asynchronous devices arrive.

Host file access is an explicit development facility. The current monitor
lends programs host files; it does not enforce the guest kernel's manifest
permissions. Keep that distinction visible, and use disposable fixtures
for tests. The CPU core still has no knowledge of host files.

- Read: calls 0–6 in [system.md](system.md), [monitor.c](../src/monitor.c).
- Done when an assembly tool can copy a small input file, handle a failed
  open and a short write, and return a checked exit status.

## 5. Grow the language, runtime and library together

Write the first compiler in C and emit the same assembly a person writes.
Keep lexer, parser, type checking and code generation separate. The current
`r0` result, stack temporaries and `r7` frame pointer are good starting
choices; no optimizing IR or register allocator is needed yet.

Do not make the entire language a single milestone:

1. Compile a function returning an integer; add variables, arithmetic,
   branches, loops and calls. Compare its result with a hand-written
   assembly program. Add source locations to errors immediately.
2. Add strings, arrays, pointers, structs and checked slices, plus the
   small runtime needed for output and startup. Build `mem`, `io` and
   `text` alongside the features they exercise.
3. Add modules, manifests and the remaining features when actual programs
   need them. Enums and `match` help the compiler and kernel; fixed point
   and function values need not delay the first console utility.

Make a small guest program read a file and write a report before attempting
the kernel. It exercises much more than arithmetic examples do. It is also
a useful place to settle error returns and arena lifetimes.

For each feature, test a working program, a rejected program and relevant
runtime failures. Check overflow in literal parsing, type sizes, frame
layout and `count * element_size`; language arithmetic wrapping does not
make wrapping an allocation size valid. Decide constant-expression typing
deliberately: the current unsigned treatment of untyped `-9 / 2` is a
surprising rule worth reconsidering before more code depends on it.

Keep the language small. Checked slices, explicit allocation and simple
modules are valuable without generics, methods or a borrow checker. They
do not make raw pointer casts or pointers into reset arenas safe. Document
those lifetimes, and keep unchecked operations easy to identify.

- Read: [language.md](language.md), [system/tc/](../system/tc/),
  [runtime.asm](../system/lib/runtime.asm).
- Done when a useful tool written in the language runs on the monitor,
  with enough structs, memory access and modules to start a small kernel.
  Extend the language as later programs justify it.

## 6. Boot a tiny guest kernel

Add privileged execution and a console output device. Start by loading a
kernel executable directly from a host file. A block-device boot path is
not required to prove the kernel can run; its entry registers can already
follow the eventual boot contract.

Write the kernel in the Terrarium language, with a small assembly startup
and port wrappers. It prints a message and halts. Keep the host monitor
available as a separate route for tools and tests.

- Read: [devices.md](devices.md#starting),
  [kruntime.asm](../system/kernel/kruntime.asm).
- Done when compiled guest code boots in privileged mode and a deliberate
  kernel fault produces a useful report.
- Leave out: process scheduling, a shell, disk boot, graphics and input.

## 7. Run one user program under the kernel

Implement the second register bank, checked user segments, `resume` and
the return to the kernel on a system call or fault. The existing design is
particularly good here: the kernel remains an ordinary loop rather than
needing interrupt entry assembly and saved trap frames.

Give one program a fixed region. Load it from an embedded image or a boot
payload supplied by the host, until the filesystem exists. Implement exit
and console output through the kernel, then console input. Validate user
buffers at the syscall boundary. Add permission checks with each operation
that acquires authority, rather than adding them after all calls exist.
Using an explicitly inherited stream is distinct from opening a new file.

- Read: [isa.md](isa.md#kernel-and-user-mode),
  [process.tr](../system/kernel/process.tr), [system.md](system.md).
- Done when the same small user executable runs under the monitor and
  under the kernel, a user fault leaves the kernel working, and attempts
  to access kernel memory or privileged instructions fail.

## 8. Read-only storage and disk boot

Add the block device, then read-only TFS traversal: validate the volume,
follow bounded chains, resolve a path and read a file. Keep the host block
adapter separate from the guest filesystem parser. A memory-backed adapter
is useful for injecting failures in tests.

Write a minimal `mkdisk` in the guest language, running on the monitor. It
needs files and the core library, not fonts or a graphical system. A tiny
temporary C image builder is also reasonable if it unblocks storage work;
it can be replaced once the guest tool works. Initially only a kernel and
one program need to go on the image.

The image builder's first acceptance test inspects the image on the host.
Only after the filesystem reader exists should a guest directory listing
be its acceptance test. This avoids a dependency hidden in the old plan.
Switch kernel loading to the disk header once both parts work.

- Read: [filesystem.md](filesystem.md),
  [mkdisk.tr](../system/tools/mkdisk.tr), [fs.tr](../system/kernel/fs.tr).
- Done when the kernel boots from a generated disk and runs a program
  loaded by path. Truncated images, invalid geometry and broken or cyclic
  chains produce errors instead of hangs or unchecked accesses.

## 9. Processes, blocking I/O and a console shell

Add the monotonic clock, timer interrupt and `wait`. Choose the clock
boundary from [time.md](time.md) now: elapsed time and UTC dates are
different things. A two-word representation avoids exposing short counter
wraparound to applications without requiring general 64-bit arithmetic in
the language. Nanosecond precision and time-zone databases can wait.

Grow the kernel in independently testable pieces:

1. A small process table, first-fit contiguous memory allocation, exit and
   cleanup. Add `spawn`, parent permission intersection and `wait`.
2. Round-robin scheduling and timer preemption. Cooperative scheduling can
   be a short intermediate step, but a busy-looping program must not stop
   the console before moving on.
3. Per-process handles with explicit ownership and reference counts. Add
   pipes, partial transfers and blocking calls. Retrying a syscall is fine
   if blocking occurs before any effect; after transferring bytes, return
   the progress rather than replaying it.
4. A tiny shell that launches a program and waits; then arguments,
   redirection and pipelines. Background jobs and richer shell syntax can
   wait until foreground use is reliable.

Fixed-size tables and waking all blocked processes are acceptable at this
scale. Isolate the wait/wake operations so targeted wakeups can replace the
scan later. Define what happens when a pipe's peer closes, a process is
killed while blocked, or a table fills up.

- Done when two programs make progress independently, a pipe transfers
  more than its buffer capacity, a child fault does not kill the shell,
  and exits release memory and handles. A scripted shell session is now
  an integration test, not the only test of those mechanisms.
- Defer compaction, threads, paging and service processes. Keep relocatable
  guest addresses, so compaction can be added without changing programs.

## 10. Writable files that can be trusted

Add create, write, truncate, directories, remove and rename incrementally.
Make every block and metadata operation propagate errors. Check disk-full
behavior and failures partway through an operation. A successful byte
count must describe bytes actually accepted under the documented storage
contract, not just bytes the kernel attempted to write.

Use throwaway images at first. Before keeping original work on them, add a
filesystem checker and define update ordering and recovery. A first version
can explicitly require clean shutdown; journaling is not a prerequisite
for a working filesystem. Specify what survives a crash before promising
that it does. Distinguish a successful write from durable storage, and
provide a flush contract when durability is needed.

Saving a document deserves its own test. Truncating the original and then
writing the replacement can lose it on failure. A temporary file plus a
replacement operation is a useful later design, but the current `rename`
does not replace existing paths and does not promise crash atomicity.
Design and test those semantics rather than assuming the current API is
already enough. A bounded editor can start with explicit backup copies.

- Done when a file survives reboot, disk-full and injected I/O errors are
  reported, and a failed save does not silently destroy the only good
  copy. Test with the checker as well as expected console output.
- Keep TFS's simple block table initially. Stable file identities, moving
  open files, handles for working directories and larger disks can follow
  demonstrated needs.

## 11. Self-hosting, when the language has settled

Port the compiler and assembler to Terrarium. Keeping the current phase
boundaries and readable assembly makes this tractable. Compare the C and
guest compiler on valid programs, invalid programs and generated output;
then build the compiler with itself and compare successive results.

This can first happen on the development monitor: a desktop, writable
guest filesystem and guest shell are not technical prerequisites. Doing
it after the kernel and some tools have exercised the language reduces
the cost of changing two implementations. Move it earlier if the compiler
is the part you most want to work on; it need not block graphics either.

The current file-for-file C port is easy to compare but makes every future
language change a double implementation. Initially that is a reasonable
trade. Later, consider freezing a small bootstrap language subset and
keeping the self-hosted compiler's source within it, so the C compiler only
needs to build that compiler. New application-language features can then
live in the guest compiler. This requires discipline about which language
features the compiler itself uses; it is not an automatic consequence of
self-hosting.

- Done when a clean build starts from C source and produces a guest
  compiler that rebuilds itself identically; later repeat it inside the
  guest OS using the source on disk.
- Byte-for-byte agreement is excellent regression evidence. Two closely
  related implementations can share a bug, so agreement does not replace
  semantic tests or independently calculated instruction encodings.

## 12. A screen, then input

This is the first required graphics work. It can begin any time after the
small kernel boots, or wait until the console system is useful.

First add a fixed-size framebuffer in one pixel format and a present
operation. Test it by saving a host image; a host window is optional. Then
show that same framebuffer in an SDL window. Keep presentation in the host
adapter rather than in the CPU or rasterizer.

Add fills, image copies and bitmap text before triangles, blending or
compressed images. A tiny compiled-in font is enough to begin; add TFN1
and `mkfont` when the font pipeline is useful. Test clipping and malformed
command lists with small pixel buffers whose results can be checked exactly.

Add keyboard and mouse events separately. Keep text entry distinct from
physical key events, as the current system does. Specify event queue
overflow: coalesce movement and resize events, and ensure losing an event
cannot leave a button permanently held. Scripted input should use the same
queue as live input.

- Read: [devices.md](devices.md), [gpu.c](../src/gpu.c),
  [input.c](../src/input.c).
- Done when one guest program draws text and a rectangle, responds to
  input, and has a repeatable framebuffer test. There are still no windows,
  desktop controls, audio or network requirements.

## 13. Windows and a small useful desktop

For the balanced route through this project, start with the WM in the
kernel. Keep its state and operations behind a small module boundary.
Moving it to userspace is a separate milestone with real prerequisites,
described below.

Consider **buffered windows from the first WM**, even while it lives in
the kernel. Keeping a window's pixels makes move, expose and redraw behavior
easier to reason about: the compositor can move its last completed picture
without waiting for the application. Begin with full composition and add
damage tracking after measuring it. This trades memory and copies for less
repaint coordination.

It needs a way to draw into a bounded offscreen surface; the current GPU
only targets the screen back buffer. Define the target's dimensions, stride
and bounds. Charge surface memory to an explicit budget: a 400 by 300
32-bit surface takes 480,000 bytes, and large windows add up quickly within
52 MiB. Start with few modest windows and a limit on total surface bytes.
One backing surface per window can work if a whole frame is submitted and
applied in one bounded synchronous operation. Incremental drawing needs
staging or another buffer if composition must keep showing the last
completed frame. Make that choice explicit rather than letting a slow
client expose a half-drawn surface.

If minimal memory is the stronger goal, the current direct-drawing model
is still viable. Its cost is visible-region tracking, redraw coordination
and behavior when a client stalls. In either model, make frame completion
explicit. Asking for the next input event should not implicitly mean that
every window belonging to the process has finished painting.

Build one window, then two overlapping windows, focus, movement, close and
resize. Add a terminal and a small editor before a large widget library.
The existing immediate-mode controls are a good fit; add common controls
when a second application needs them.

- Read: [system.md](system.md#windows), [wm.tr](../system/kernel/wm.tr),
  [gfx.tr](../system/lib/gfx.tr), [ui.tr](../system/lib/ui.tr).
- Done when you can edit, save, compile and run a small program; another
  window keeps working when that program loops or faults. Test slow clients,
  resize, queue overflow and closing a window during interaction.
- The terminal, editor, launcher, monitor and one drawing application are
  enough to exercise the architecture. Rebuilding every demo is optional.

## 14. Audio, network and richer formats

These are separate branches after processes and blocking I/O work. Neither
requires the other, and neither belongs in the first CPU milestone.

**Audio:** queue raw PCM and play a tone first. Keep a silent implementation
for tests. A single mono rate is a good first limit, but expose it through
the API rather than baking it into applications. Add TSN1 and `mksound`
after streaming works. The current shared sample queue serializes audio
from applications; it is not a mixer. Add per-client stream handles and
mixing when simultaneous sound matters. Do not make queued sample memory
outlive its owner accidentally: copy it or define ownership explicitly.

**Network:** begin with numeric loopback addresses and a small echo client
and server, then stream integration and readiness. Use the host's TCP
implementation; there is no need for a guest TCP/IP stack to keep
Terrarium's own API. Add hostname resolution without blocking the machine,
using a bounded asynchronous host operation. Test partial sends, a failed
connect, orderly close and abrupt disconnect as distinct cases.

**Images and sounds:** raw pixels and samples are enough for initial
applications. TIM1's independent tiles and TSN1's independent blocks are
good later fits for bounded memory. Put codecs in guest libraries and
outside-world conversion in tools. Those import tools do not require
legacy formats throughout the guest OS. Validate dimensions, lengths,
offsets and multiplication overflow before allocation or decoding.

- Read: [formats.md](formats.md), [devices.md](devices.md),
  [system/tools/](../system/tools/).
- Done per branch: a repeatable tone/queue test; a client/server exchange
  with failure cases; codec round trips and malformed-file tests. Do not
  make finishing all three a condition for using the desktop.

## Boundaries that leave room to grow

### A userspace WindowServer

WM placement and window buffering are independent decisions. Keeping pixels
does not require a userspace server, and moving the WM does not by itself
solve repaint coordination.

To move it later, first add bounded IPC with owned endpoints, copied
messages, disconnect behavior and deliberate handle delegation. An
inherited connection to the server is enough for initial discovery. Give
the server display/input authority separately from an ordinary client's
permission to open a window. Put the transport behind `gfx`, so programs
continue to use window and event operations.

Commands must describe protocol data, not kernel `Process` pointers or
unchecked pointers into another program's memory. Initially copy commands
and referenced text/image data with explicit size limits. Later introduce
owned buffer handles and offsets if copies become expensive. Shared
surfaces require actual shared-memory support: today's one private
contiguous region per process does not supply that automatically. Define
lifetime, relocation and synchronization before promising zero-copy use.

Only then move window state, composition and input routing. Test server
exit and client cleanup. Doing the server up front is reasonable if IPC
and OS service design are the main learning goal; it makes the first
desktop depend on all of those mechanisms. For a broad solo rewrite,
the kernel WM is the smaller first step.

### Memory and resource accounting

Keep the manifest and one contiguous reservation per process. They make
admission and isolation understandable. Start with first-fit allocation
and add compaction only when fragmentation is observable. Keep user
addresses distinct from physical addresses and do not retain translated
pointers across operations that can relocate a process.

A reservation prevents another process from taking those bytes; an
application can still exhaust its own arena. Arena reset can leave dangling
pointers, and a long-lived arena can retain unused allocations. Provide a
fallible allocation operation where recovery matters. Track allocation
peaks and minimum stack pointer explicitly if reporting high-water marks:
scanning for nonzero memory misses zero writes and memory later cleared.

A recovery memory reserve alone cannot guarantee a recovery tool starts.
It also needs a process slot, handles and a way to be launched. Reserve
those or provide a small already-running control path before making that
guarantee. Keep broad manifest permissions first; more precise authority
can grow through deliberately granted handles rather than ambient access.

### Time, devices and repeatability

Use a clock interface with real and simulated implementations. In tests,
advance virtual time predictably while executing and jump to the next
scheduled event when the machine is idle. Fix the wall clock separately.
Record or supply input and network results when those affect a scenario.
`--date`, headless output and a silent audio device alone do not make
scheduling repeatable.

Synchronous device calls are a good initial simplification only when their
work is bounded. The kernel is not preempted, so a host DNS lookup or an
unbounded drawing command can stall every process. Enforce work limits
within expensive operations, not only after finishing them. Device
completion also needs precise meaning: accepting audio into a queue does
not mean the sound has finished playing.

### Debugging and performance

Keep instruction traces, symbols, syscall traces and resource counters.
Add source spans to compiler diagnostics and eventually a PC-to-source
map. Prefer explicit per-subsystem state objects over growing shared global
state; a standalone compiler can still use one instance of its context.
Name addresses and sizes by their units and ownership, especially across
CPU, kernel and GPU boundaries.

Keep the interpreter as the executable reference. Profile real editor,
compiler and graphics workloads before adding an optimizer or native
translation. A faster engine must preserve traps, arithmetic, observable
cycle accounting and interrupt opportunities. Threads, SMP, a general VFS,
dynamic linking and a generic service framework can wait for an actual
program that needs them.

## What the existing implementation teaches

The idea is coherent: the machine constrains the language, the language
builds the OS, and the OS supports editing and building its own programs.
Readable assembly, the `resume` model, one pixel format, explicit budgets,
small formats and guest-written tools are strengths worth retaining.
Streams, handles and pipes are useful abstractions even when familiar;
they do not require adopting a legacy compatibility API around them.

These are the main areas I would change or strengthen in a rewrite:

| Area and current example | Recommendation |
| ------------------------ | -------------- |
| CPU: [machine.c](../src/machine.c), `machine_step` | Preserve the small interpreter. Add state-level fault tests: an `in` targeting `sp` currently reads the port before validating the resulting stack pointer, so a failing instruction can consume input. Settle validation/commit semantics when adding devices. |
| Compiler: [system/tc/](../system/tc/) and [asm.tr](../system/lib/asm.tr) | Preserve the phases and assembly boundary. Improve diagnostics, checked size calculations, negative tests and ownership of compiler state before optimizing generated code. |
| Kernel: [process.tr](../system/kernel/process.tr), [syscall.tr](../system/kernel/syscall.tr) | Preserve the simple scheduler and handle model. Introduce permissions with calls, separate subsystem handlers as they grow, and document retry/partial-progress rules. Do not make an unlisted new operation implicitly unrestricted. |
| Storage: [fs.tr](../system/kernel/fs.tr), `write_at`, `read_meta`, `set_next_block` | Several disk-transfer results are ignored; write counts can indicate success despite failure. Error propagation and recovery tests matter before the editor stores valuable work. |
| Desktop: [wm.tr](../system/kernel/wm.tr), `finished_drawing`, `post` | Replace implicit paint completion and specify queue overflow behavior. Consider backing surfaces before a userspace move; the current full queue silently drops new events, including release events. |
| Libraries and apps: [io.tr](../system/lib/io.tr), [text.tr](../system/lib/text.tr), [edit.tr](../system/bin/edit.tr) | Keep small focused modules and the editor's gap buffer. Make truncated input, failed reads, full buffers and failed saves explicit. UTF-8 byte offsets, code points and displayed cells are different quantities; choose a small documented text model and validate malformed input consistently. |
| Asset tools: [mkdisk.tr](../system/tools/mkdisk.tr), [mkimage.tr](../system/tools/mkimage.tr), [mksound.tr](../system/tools/mksound.tr) | Guest-language tools are a strong way to exercise the language. Whole-file buffering is fine under stated limits; later stream large inputs. Keep advertised limits within their manifests: the current `mkdisk` accepts 64 MiB but reserves only 48 MiB while allocating the whole image plus inputs. |
| Host devices: [devices.c](../src/devices.c), [gpu.c](../src/gpu.c), [net.c](../src/net.c) | Keep host integration out of the CPU. Make worst-case work bounded; DNS is currently synchronous, and GPU budget enforcement should happen during costly work. |
| Build and tests: [Makefile](../Makefile), [tests/run.sh](../tests/run.sh) | Keep the source bootstrap and disposable disk sessions. Add rejection and failure-injection cases, check process status as well as output, and automate a few desktop scenarios. Compiler parity and screenshots complement, rather than replace, assertions about behavior. |

This is an architectural review and a set of concrete inspection findings,
not an exhaustive correctness audit of every application. The point of the
rewrite is to make these contracts and failure paths understandable while
the system is still small.

## Keep progress visible

Keep the old implementation runnable as a reference, and keep a short
decision log when intentionally changing its behavior. Use its programs as
test inputs without treating accidental behavior as a specification.

Useful stopping points are a tested CPU; an assembler you can use; a
compiled console tool; a kernel running one program; a shell with persistent
files; and finally an editor/build/run desktop. Each is a complete piece
of the larger project. Audio, networking, self-hosting and performance
work can be chosen around those points without making every subsystem a
prerequisite for the next visible result.
