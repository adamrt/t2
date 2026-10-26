# Terrarium Assembly

Each line holds at most one statement, optionally preceded by a label.
A `;` starts a comment that runs to the end of the line.

```asm
; Print "Hi" and exit.

start:
    r0 = 1              ; stdout
    r1 = message
    r2 = 2
    sys 2               ; write
    r0 = 0
    sys 0               ; exit

.data
message:
    .byte "Hi"
```

## Names and values

A name starts with a letter or `_`, followed by letters, digits and `_`.
Register names (`r0`–`r7`, `sp`) and keywords (`if`, `goto`, `call`,
`return`, `push`, `pop`, `copy`, `fill`, `sys`, `halt`, `u8`, `u16`, `u32`)
can't be used as names.

A **value** is a number, a character, or a name, optionally preceded by `-`:

```asm
42      0x2a      'A'      '\n'      -1      BUFFER_SIZE      message
```

Character escapes are `\n`, `\t`, `\r`, `\0`, `\\`, `\'` and `\"`.

A **label** (`name:`) names an address in the current section. A
**constant** (`NAME = value`) names a number. A constant must be defined
before it is used.

## Instructions

`rd`, `rs` and `rt` stand for registers. `imm` is a number or constant that
fits in 16 signed bits (−32768 to 32767).

### Moving values

```asm
r0 = 42             ; load a number (one instruction if it fits in 16 bits, else two)
r0 = message        ; load a label's address (always two instructions)
r0 = r1             ; copy a register
```

### Arithmetic

```asm
r0 = r1 + r2        ; or + - * / % & | ^ << >>
r0 = r1 + 4         ; the second operand can be imm
r0 += r1            ; shorthand for r0 = r0 + r1
r0 -= 1
```

`/`, `%` and `>>` are unsigned. Their signed versions are `/s`, `%s` and
`>>s`:

```asm
r0 = r1 /s r2
r0 >>s= 4
```

### Memory

```asm
r0 = [r1]           ; load 32 bits
r0 = [r1 + 8]       ; with an offset (imm)
r0 = [sp - 4]
r0 = u8 [r1]        ; load a byte, zero-extended
r0 = u16 [r1 + 2]   ; load 16 bits, zero-extended
[r1 + 8] = r0       ; store 32 bits
u8 [r1] = r0        ; store the low byte
u16 [r1] = r0       ; store the low 16 bits
```

`u32 [...]` is also accepted and means the same as `[...]`.

### Control flow

```asm
goto loop
if r0 == r1 goto done       ; == != < <= > >=   (unsigned)
if r0 <s r1 goto negative   ; <s <=s >s >=s     (signed)
if r0 == 0 goto empty       ; zero is the only number allowed
if r0 != 0 goto more
call print
call r3                     ; call the address in a register
return
```

### Stack

```asm
push r4, r5         ; pushes r4, then r5
pop r5, r4          ; pops r5, then r4
```

### Bulk memory

```asm
copy r0, r1, r2     ; copy r2 bytes from [r1] to [r0]
fill r0, r1, r2     ; set r2 bytes at [r0] to the low byte of r1
```

### System

```asm
sys 2               ; system call
halt
```

## Sections

| Directive | Holds                                         |
| --------- | --------------------------------------------- |
| `.code`   | instructions (the default)                    |
| `.data`   | initialized data                              |
| `.bss`    | zero-filled data, which takes no file space   |

Code can't be read as data, so strings and tables go in `.data`.

## Directives

```asm
.byte 1, 2, 'x', "text"     ; bytes and strings (no terminator is added)
.word 1, -1, message        ; 32-bit values, including label addresses
.zero 256                   ; zero bytes
.align 4                    ; pad with zeros to a multiple of 4
.memory 0x100000            ; total memory the program needs
.stack 0x4000               ; stack size within that memory
```

`.byte` and `.word` belong in `.data`. `.zero` and `.align` work in `.data`
and `.bss`.

`.memory` covers everything: code, data, bss, heap and stack. Without it, a
program gets exactly enough for its code, data, bss and stack, and no heap.
The stack defaults to 64 KiB.

The program starts at the label `start`, which must be in `.code`.

## Encoding pseudo-instructions

Two statements expand to more than one instruction:

- `rd = value` becomes `li`, plus `lih` when the value doesn't fit in 16 bits.
  A label always takes both.
- `push` and `pop` with several registers become one instruction per register.

Every other statement is exactly one instruction.
