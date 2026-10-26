; Exercises every instruction. Exits with the number of the first failing
; check, or prints "ok".

start:
    r6 = r0             ; heap start
    r7 = r1             ; heap end

    r2 = 1              ; li and lih
    r0 = -1
    r1 = 0xffffffff
    if r0 != r1 goto fail
    r2 = 2
    r0 = 0x12345678
    r1 = 0x1234
    r1 <<= 16
    r1 |= 0x5678
    if r0 != r1 goto fail

    r2 = 3              ; arithmetic
    r0 = 7
    r1 = 5
    r3 = r0 + r1
    r4 = 12
    if r3 != r4 goto fail
    r2 = 4
    r3 = r1 - r0
    r4 = -2
    if r3 != r4 goto fail
    r2 = 5
    r3 = r0 * r1
    r4 = 35
    if r3 != r4 goto fail
    r2 = 6
    r3 = -7
    r3 = r3 /s r1
    r4 = -1
    if r3 != r4 goto fail
    r2 = 7
    r3 = -7
    r3 = r3 %s r1
    r4 = -2
    if r3 != r4 goto fail
    r2 = 8
    r3 = -7
    r3 = r3 / r1
    r4 = 858993457
    if r3 != r4 goto fail
    r2 = 9
    r3 = 17
    r3 %= r1
    r4 = 2
    if r3 != r4 goto fail
    r2 = 10
    r3 = 0x80000000
    r4 = -1
    r5 = r3 /s r4
    if r5 != r3 goto fail
    r2 = 11
    r5 = r3 %s r4
    if r5 != 0 goto fail

    r2 = 12             ; logic and shifts
    r0 = 0xf0f0
    r1 = 0x0ff0
    r3 = r0 & r1
    r4 = 0x00f0
    if r3 != r4 goto fail
    r2 = 13
    r3 = r0 | r1
    r4 = 0xfff0
    if r3 != r4 goto fail
    r2 = 14
    r3 = r0 ^ r1
    r4 = 0xff00
    if r3 != r4 goto fail
    r2 = 15
    r3 = 1
    r3 = r3 << 33       ; shift amounts use the low 5 bits
    r4 = 2
    if r3 != r4 goto fail
    r2 = 16
    r3 = -16
    r3 >>= 2
    r4 = 0x3ffffffc
    if r3 != r4 goto fail
    r2 = 17
    r3 = -16
    r3 >>s= 2
    r4 = -4
    if r3 != r4 goto fail
    r2 = 18
    r3 = r0 & -1        ; immediates are sign-extended
    if r3 != r0 goto fail

    r2 = 19             ; memory
    r0 = buffer
    r1 = 0x11223344
    [r0 + 4] = r1
    r3 = u8 [r0 + 4]
    r4 = 0x44
    if r3 != r4 goto fail
    r2 = 20
    r3 = u16 [r0 + 6]
    r4 = 0x1122
    if r3 != r4 goto fail
    r2 = 21
    r1 = 0xaabbccdd
    u8 [r0 + 4] = r1
    u16 [r0 + 6] = r1
    r3 = [r0 + 4]
    r4 = 0xccdd33dd
    if r3 != r4 goto fail
    r2 = 22
    r0 += 8
    r3 = u32 [r0 - 4]
    if r3 != r4 goto fail
    r2 = 23
    r0 = table
    r1 = [r0 + 4]
    r3 = second
    if r1 != r3 goto fail
    r2 = 24
    r1 = [r0 + 8]
    r3 = -5
    if r1 != r3 goto fail

    r2 = 25             ; branches
    r0 = -1
    r1 = 1
    if r0 < r1 goto fail
    r2 = 26
    if r1 >s r0 goto signed_ok
    goto fail
signed_ok:
    r2 = 27
    if r0 <=s r1 goto le_ok
    goto fail
le_ok:
    r2 = 28
    if r1 >= r0 goto fail
    r2 = 29
    if r0 <= r1 goto fail
    r2 = 30
    if r1 > r0 goto fail
    r2 = 31
    if r0 >=s r1 goto fail
    r2 = 32
    if r1 <s r0 goto fail
    r2 = 33
    r3 = 0
    if r3 != 0 goto fail
    if r1 == 0 goto fail
    if r1 == r0 goto fail
    if r1 != r1 goto fail

    r2 = 34             ; calls and the stack
    r0 = 20
    call double
    r1 = 40
    if r0 != r1 goto fail
    r2 = 35
    r3 = double
    call r3
    r1 = 80
    if r0 != r1 goto fail
    r2 = 36
    r0 = 1
    r1 = 2
    push r0, r1
    pop r0, r1
    r3 = 2
    if r0 != r3 goto fail
    r2 = 37
    r4 = sp
    push sp
    pop r5
    if r4 != r5 goto fail

    r2 = 38             ; bulk memory
    r0 = buffer
    r1 = 'x'
    r3 = 8
    fill r0, r1, r3
    r1 = u8 [r0 + 7]
    r4 = 'x'
    if r1 != r4 goto fail
    r2 = 39
    r1 = message
    r3 = 5
    copy r0, r1, r3
    r1 = r0 + 1         ; overlapping copy shifts the bytes right
    r3 = 4
    copy r1, r0, r3
    r1 = u8 [r0 + 4]
    r4 = 'l'
    if r1 != r4 goto fail
    r1 = u8 [r0 + 1]
    r4 = 'h'
    if r1 != r4 goto fail

    r2 = 40             ; bss starts zeroed, heap bounds are sane
    r0 = zeroed
    r1 = [r0 + 60]
    if r1 != 0 goto fail
    r2 = 41
    if r6 >= r7 goto fail
    r0 = 0x1000
    r1 = r7 - r6
    if r1 < r0 goto fail
    r0 = [r6]
    [r7 - 4] = r0

    r0 = 1
    r1 = ok
    r2 = 3
    sys 2
    r0 = 0
    sys 0

fail:
    r0 = r2
    sys 0

double:
    r0 += r0
    return

.data
message:
    .byte "hello"
ok:
    .byte "ok\n"
    .align 4
table:
    .word 1, second, -5
second:
    .word 0

.bss
buffer:
    .zero 16
zeroed:
    .zero 64

.memory 0x20000
.stack 0x1000
