.text
.global _start
_start:
    mov x8, #93        // __NR_exit (aarch64)
    mov x0, #42        // exit code
    svc #0
