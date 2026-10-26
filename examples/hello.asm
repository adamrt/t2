; Print a greeting and exit.

start:
    r0 = 1              ; stdout
    r1 = message
    r2 = message_end
    r2 -= r1
    sys 2               ; write
    r0 = 0
    sys 0               ; exit

.data
message:
    .byte "Hello, Terrarium!\n"
message_end:
