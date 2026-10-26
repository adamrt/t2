; fault: stack overflow
start:
    sp -= 0x4000
    halt
.stack 0x1000
.memory 0x20000
