; fault: alignment
start:
    r0 = buffer
    r1 = [r0 + 2]
    halt
.bss
buffer:
    .zero 8
