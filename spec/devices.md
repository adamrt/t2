# Devices

Devices are reached through ports with `in` and `out`, in kernel mode. Every
device finishes its work before the `out` that starts it returns, so there
is nothing to wait for.

Memory addresses given to devices are physical.

## Console

The host's terminal.

| Port   |   | Meaning                                                      |
| ------ | - | ------------------------------------------------------------ |
| `0x20` | w | write one byte                                               |
| `0x21` | r | read one byte; `0xffffffff` if none is waiting, `0xfffffffe` once input has ended |

Interrupt bit 1 is raised when input arrives or ends.

## Clock and timer

| Port   |   | Meaning                                                      |
| ------ | - | ------------------------------------------------------------ |
| `0x22` | r | microseconds since the machine started (wraps after 71 minutes) |
| `0x23` | w | raise interrupt bit 0 after this many microseconds; 0 cancels |

## Disk

A block device of 4096-byte blocks.

| Port   |   | Meaning                                         |
| ------ | - | ----------------------------------------------- |
| `0x30` | w | first block                                     |
| `0x31` | w | physical memory address                         |
| `0x32` | w | number of blocks                                |
| `0x33` | w | 1 reads blocks into memory, 2 writes them       |
| `0x34` | r | 0 if the last command worked, 1 if it failed    |
| `0x35` | r | number of blocks on the disk                    |

## Starting

`build/terrarium boot disk.img` reads the disk's first block (see
[filesystem.md](filesystem.md)) to find the kernel, an ordinary executable.
The kernel's data, heap and stack go at the start of RAM, with its code
right after. Its data space covers all of RAM, so its data address `A` is
physical address `A - 0x10000`. It starts in kernel mode with:

| Register | Value                                         |
| -------- | --------------------------------------------- |
| r0, r1   | heap start and end, as for any program        |
| r2       | first free physical address, a multiple of 4096 |
| r3       | the size of RAM                               |
