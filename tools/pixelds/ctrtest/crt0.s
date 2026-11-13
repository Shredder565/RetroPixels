@ PixelDS 3DS test program start-up: clear BSS, run main, exit.
	.section .text.start, "ax"
	.arm
	.global _start
_start:
	ldr r0, =__bss_start
	ldr r1, =__bss_end
	mov r2, #0
1:	cmp r0, r1
	strlo r2, [r0], #4
	blo 1b
	bl main
	svc 0x03
	b .
	.pool
