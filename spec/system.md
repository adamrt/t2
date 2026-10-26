# System Calls

A program asks the kernel for things with `sys N`. Arguments go in r0–r6
and the result comes back in r0; every other register keeps its value.
Failures return `-1` (`0xffffffff`). Calls 0–6 also work on the development
monitor, so the same tools run on both.

| N  | Call       | Arguments                                         | Result                     |
| -- | ---------- | ------------------------------------------------- | -------------------------- |
| 0  | exit       | r0 status                                         |                            |
| 1  | read       | r0 handle, r1 buffer, r2 length                   | bytes read, 0 at the end   |
| 2  | write      | r0 handle, r1 bytes, r2 length                    | bytes written              |
| 3  | open       | r0 path, r1 length, r2 mode                       | handle                     |
| 4  | close      | r0 handle                                         | 0                          |
| 5  | args       |                                                   | number of arguments        |
| 6  | arg        | r0 index, r1 buffer, r2 length                    | the argument's length      |
| 7  | spawn      | r0 path, r1 length, r2 args, r3 length, r4 input, r5 output, r6 errors | process id |
| 8  | wait       | r0 process id                                     | its exit status            |
| 9  | pipe       |                                                   | reading handle; writing handle in r1 |
| 10 | info       | r0 path, r1 length, r2 buffer for kind and size   | 0                          |
| 11 | list       | r0 path, r1 length, r2 buffer, r3 length          | bytes of records           |
| 12 | remove     | r0 path, r1 length                                | 0                          |
| 13 | make_dir   | r0 path, r1 length                                | 0                          |
| 14 | set_dir    | r0 path, r1 length                                | 0                          |
| 15 | get_dir    | r0 buffer, r1 length                              | the path's length          |
| 16 | time       |                                                   | milliseconds since start   |
| 17 | sleep      | r0 milliseconds                                   | 0                          |
| 18 | processes  | r0 buffer, r1 length                              | bytes of records           |
| 19 | kill       | r0 process id                                     | 0                          |
| 20 | memory     |                                                   | bytes for programs; free bytes in r1 |

## Handles

A handle is a small number naming something a process can read or write:
the console, a file, or one end of a pipe. Every process starts with
handles 0, 1 and 2 for its input, output and errors, chosen by whoever
spawned it.

`open` modes are 0 to read, 1 to create or empty a file and write it, and
2 to write at its end. Reading a pipe or the console waits until there is
something to read; writing a full pipe waits until there is room.

## Paths

Paths are separated by `/`. A path that doesn't start with `/` is relative
to the process's directory, which it gets from its parent and changes with
`set_dir`. `.` and `..` mean what you'd expect.

## Programs

`spawn` starts an executable with its own memory, which it never shares.
Its arguments are the bytes in `args`, each ending with a zero byte. The new
process gets the parent's directory and the three given handles. A parent
collects an ended child's status with `wait`.

## Records

`list` fills its buffer with 64-byte records: a name of up to 48 bytes
padded with zeros, then the kind (1 file, 2 directory), the size, and 8 zero
bytes.

`processes` fills its buffer with 48-byte records: the id, the parent's id,
the state (1 ready, 2 waiting, 3 ended), the memory reserved, the CPU cycles
used, and a name of up to 28 bytes.
