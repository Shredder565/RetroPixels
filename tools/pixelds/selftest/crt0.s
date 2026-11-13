@ PixelDS self-test: start-up code for both processors (direct boot leaves
@ the program loaded; this sets the stacks, clears .bss and calls main).
    .section .crt0, "ax"
    .arm
    .global _start
_start:
    mov r0, #0x12           @ IRQ mode
    msr cpsr_c, r0
    ldr sp, =__sp_irq
    mov r0, #0x13           @ SVC mode
    msr cpsr_c, r0
    ldr sp, =__sp_svc
    mov r0, #0x1f           @ SYS mode
    msr cpsr_c, r0
    ldr sp, =__sp_usr
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b
    ldr r0, =main
    mov lr, pc
    bx r0
2:  b 2b
