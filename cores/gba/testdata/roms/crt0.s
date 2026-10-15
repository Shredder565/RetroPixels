@ Start-up code for RetroPixels' GBA test ROMs (original code): cartridge header,
@ .data/.bss set-up, and an interrupt handler that acknowledges IF, sets the
@ BIOS interrupt flags and calls irq_hook.
    .section .text.start, "ax"
    .arm
    .global _start
_start:
    b real_start
    .space 0xa0 - 4
    .ascii "RPTEST      "      @ 0xa0 title
    .ascii "XXXX"              @ 0xac game code (patched)
    .ascii "01"                @ 0xb0 maker
    .byte 0x96, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 @ 0xb2..0xbf
    .space 0x20                @ 0xc0..0xdf: the GPIO port lives at 0xc4-0xc9
real_start:
    @ copy .data, clear .bss
    ldr r0, =__data_lma
    ldr r1, =__data_start
    ldr r2, =__data_end
1:  cmp r1, r2
    ldrlo r3, [r0], #4
    strlo r3, [r1], #4
    blo 1b
    ldr r1, =__bss_start
    ldr r2, =__bss_end
    mov r3, #0
2:  cmp r1, r2
    strlo r3, [r1], #4
    blo 2b
    ldr r0, =irq_handler
    ldr r1, =0x03007ffc
    str r0, [r1]
    ldr r0, =main
    mov lr, pc
    bx r0
3:  b 3b

    .global irq_handler
    .section .iwram, "ax"
    .arm
irq_handler:
    ldr r1, [r0, #0x200]
    and r1, r1, r1, lsr #16
    add r2, r0, #0x200
    strh r1, [r2, #2]
    ldr r2, =0x03007ff8
    ldrh r3, [r2]
    orr r3, r3, r1
    strh r3, [r2]
    ldr r2, =irq_flags
    ldr r3, [r2]
    orr r3, r3, r1
    str r3, [r2]
    ldr r2, =irq_hook
    ldr r2, [r2]
    cmp r2, #0
    bxeq lr
    stmfd sp!, {lr}
    mov r0, r1
    mov lr, pc
    bx r2
    ldmfd sp!, {lr}
    bx lr
    .pool
