; fault: data address
start:
    r0 = buffer
    r1 = 0x7fffffff
    copy r0, r0, r1
    halt
.bss
buffer:
    .zero 8
