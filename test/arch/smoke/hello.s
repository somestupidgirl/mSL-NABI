.text
.global _start
_start:
    mov x8, #64          // write
    mov x0, #1           // stdout
    adr x1, msg
    mov x2, #14
    svc #0
    mov x8, #93          // exit
    mov x0, #0
    svc #0
msg: .ascii "hello arm64!\n\0"
