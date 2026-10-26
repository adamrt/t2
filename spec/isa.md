# Terrarium CPU

A 32-bit little-endian CPU with eight general registers, a stack pointer, and
39 operations. Every instruction is one 32-bit word.

## Registers

| Name    | Number | Use                                 |
| ------- | ------ | ----------------------------------- |
| r0–r7   | 0–7    | General purpose                     |
| sp      | 8      | Stack pointer, grows down           |
| pc      | —      | Program counter, not addressable    |

Register numbers 9–15 are illegal.

## Memory

The machine has 52 MiB of RAM and 12 MiB of VRAM (VRAM arrives with the video
device).

A program sees two separate address spaces:

- **Code** holds instructions. It can only be executed, never read or written.
- **Data** holds everything else: globals, the heap, and the stack.

Both spaces begin at address `0x10000`. Any access below that address faults,
so a null pointer, or a small offset from one, can never touch real memory.
Every access is checked against the size of its space.

Loads and stores must be naturally aligned: 16-bit accesses on even addresses,
32-bit accesses on multiples of 4. The pc must be a multiple of 4.

The stack has a limit. Any instruction that moves sp below the limit faults.

## Encoding

```
 31      24 23  20 19  16 15  12 11           0
+----------+------+------+------+--------------+
|    op    |  a   |  b   |  c   |   (unused)   |   R: three registers
+----------+------+------+------+--------------+
|    op    |  a   |  b   |       imm16         |   I: two registers, 16-bit immediate
+----------+------+------+---------------------+
|    op    |              imm24                |   J: 24-bit word address
+----------+-----------------------------------+
```

`imm16` is sign-extended. Unused fields must be zero. Opcode `0x00` is
illegal, so jumping into zeroed memory faults immediately.

## Operations

In the tables, `a`, `b` and `c` are the registers named by those fields and
`imm` is the sign-extended immediate.

### Constants

| Op     | Name | Effect                                   |
| ------ | ---- | ---------------------------------------- |
| `0x01` | li   | `a = imm`                                |
| `0x02` | lih  | `a = imm << 16 \| (a & 0xffff)`          |

`li` followed by `lih` loads any 32-bit value.

### Arithmetic

Each operation has a register form and an immediate form. The immediate form
is the register opcode plus `0x10`, and uses `imm` in place of `c`.

| Reg    | Imm    | Name | Effect                         |
| ------ | ------ | ---- | ------------------------------ |
| `0x10` | `0x20` | add  | `a = b + c`                    |
| `0x11` | `0x21` | sub  | `a = b - c`                    |
| `0x12` | `0x22` | mul  | `a = b * c` (low 32 bits)      |
| `0x13` | `0x23` | div  | `a = b / c`, unsigned          |
| `0x14` | `0x24` | divs | `a = b / c`, signed            |
| `0x15` | `0x25` | rem  | `a = b % c`, unsigned          |
| `0x16` | `0x26` | rems | `a = b % c`, signed            |
| `0x17` | `0x27` | and  | `a = b & c`                    |
| `0x18` | `0x28` | or   | `a = b \| c`                   |
| `0x19` | `0x29` | xor  | `a = b ^ c`                    |
| `0x1a` | `0x2a` | shl  | `a = b << (c & 31)`            |
| `0x1b` | `0x2b` | shr  | `a = b >> (c & 31)`, logical   |
| `0x1c` | `0x2c` | sar  | `a = b >> (c & 31)`, arithmetic|

Arithmetic wraps. Division by zero faults. Signed division rounds toward zero;
`0x80000000 / -1` gives `0x80000000` and remainder 0.

A move is `add` with an immediate of zero.

### Memory

The address is `b + imm`.

| Op     | Name | Effect                         |
| ------ | ---- | ------------------------------ |
| `0x30` | ld   | `a = u32 [b + imm]`            |
| `0x31` | ld16 | `a = u16 [b + imm]`, zero-extended |
| `0x32` | ld8  | `a = u8 [b + imm]`, zero-extended  |
| `0x34` | st   | `u32 [b + imm] = a`            |
| `0x35` | st16 | `u16 [b + imm] = a`            |
| `0x36` | st8  | `u8 [b + imm] = a`             |

### Branches

A taken branch sets `pc = pc + 4 + imm * 4`, relative to the next
instruction, so a branch reaches 32768 instructions in either direction.

| Op     | Name | Taken when                 |
| ------ | ---- | -------------------------- |
| `0x40` | beq  | `a == b`                   |
| `0x41` | bne  | `a != b`                   |
| `0x42` | blt  | `a < b`, unsigned          |
| `0x43` | bge  | `a >= b`, unsigned         |
| `0x44` | blts | `a < b`, signed            |
| `0x45` | bges | `a >= b`, signed           |
| `0x46` | bz   | `a == 0`                   |
| `0x47` | bnz  | `a != 0`                   |

### Jumps and the stack

| Op     | Name  | Format | Effect                                     |
| ------ | ----- | ------ | ------------------------------------------ |
| `0x50` | jmp   | J      | `pc = imm24 * 4`                           |
| `0x51` | call  | J      | push the next pc; `pc = imm24 * 4`         |
| `0x52` | callr | R      | push the next pc; `pc = a`                 |
| `0x53` | ret   | R      | pop pc                                     |
| `0x60` | push  | R      | `sp = sp - 4; u32 [sp] = a`                |
| `0x61` | pop   | R      | `a = u32 [sp]; sp = sp + 4`                |

`push sp` stores the value sp had before the push.

### Bulk memory

| Op     | Name | Effect                                              |
| ------ | ---- | --------------------------------------------------- |
| `0x70` | copy | copy `c` bytes from address `b` to address `a`      |
| `0x71` | fill | set `c` bytes at address `a` to the low byte of `b` |

`copy` works correctly when the ranges overlap. The whole range is checked
before anything is written, so a fault leaves memory unchanged.

### System

| Op     | Name | Effect                                         |
| ------ | ---- | ---------------------------------------------- |
| `0x80` | sys  | system call number `imm16 & 0xffff`            |
| `0xff` | halt | stop the machine                               |

## Cycles

The machine counts cycles. Every instruction costs one cycle, except `copy`
and `fill`, which cost `1 + c / 4`.

## Faults

| Fault        | Cause                                                  |
| ------------ | ------------------------------------------------------ |
| illegal      | unknown opcode, register number above 8, unused bits set |
| code address | pc misaligned, below `0x10000`, or past the end of code |
| data address | access below `0x10000` or past the end of data         |
| alignment    | misaligned 16- or 32-bit load or store                 |
| divide       | division or remainder by zero                          |
| stack        | sp moved below the stack limit                         |

A faulting instruction has no effect.

## Not yet designed

Kernel mode, interrupts, the timer, device ports, and `wait`. Until then,
programs run on a development monitor that loads one executable and answers
its system calls (see [executable.md](executable.md)).
