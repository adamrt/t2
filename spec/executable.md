# Executables

An executable is a header followed by the program's code and initialized
data. All fields are little-endian 32-bit words.

| Offset | Field  | Meaning                                         |
| ------ | ------ | ----------------------------------------------- |
| 0      | magic  | the bytes `TRX1`                                |
| 4      | entry  | code address where execution starts             |
| 8      | code   | size of the code in bytes, a multiple of 4      |
| 12     | data   | size of the initialized data in bytes           |
| 16     | bss    | size of the zero-filled data in bytes           |
| 20     | stack  | size of the stack in bytes                      |
| 24     | memory | total memory the program needs                  |

The code follows at offset 28, then the data. Nothing else is in the file.

## Memory

The loader reserves `memory` bytes or refuses to start the program. The code
takes `code` bytes, and the rest becomes the data space:

```
0x10000                                                         top
  | data | bss |             heap               |     stack     |
                ^ heap start          heap end ^       sp start ^
```

- `data` is copied from the file.
- `bss` starts at the data size rounded up to 4.
- The heap starts at the end of bss, rounded up to 4, and ends where the stack
  begins.
- The stack takes the last `stack` bytes. Its bottom is the stack limit.

Everything outside `data` starts at zero. The program never gets more memory;
it divides the heap however it likes.

## Starting

| Register | Value          |
| -------- | -------------- |
| pc       | `entry`        |
| sp       | top            |
| r0       | heap start     |
| r1       | heap end       |
| r2–r7    | 0              |

## Development system calls

Until Terrarium has a kernel, the development monitor (`build/terrarium`)
runs one program and answers these system calls itself. They let tools work
with files on the host.

| Number | Call  | Arguments                                   | Result in r0                      |
| ------ | ----- | ------------------------------------------- | --------------------------------- |
| 0      | exit  | r0 status                                   | does not return                   |
| 1      | read  | r0 file, r1 buffer, r2 length               | bytes read, 0 at the end          |
| 2      | write | r0 file, r1 buffer, r2 length               | bytes written                     |
| 3      | open  | r0 path, r1 path length, r2 mode            | file                              |
| 4      | close | r0 file                                     | 0                                 |
| 5      | args  |                                             | number of arguments               |
| 6      | arg   | r0 index, r1 buffer, r2 buffer length       | length of the argument            |

- Failures return `-1` (`0xffffffff`).
- Files 0, 1 and 2 are standard input, output and error.
- Mode 0 opens for reading. Mode 1 creates or truncates for writing.
- `arg` copies at most the buffer length and returns the full length, so a
  result larger than the buffer means the argument was cut short.
- Argument 0 is the first argument after the program's path.
- Every register except r0 keeps its value.
