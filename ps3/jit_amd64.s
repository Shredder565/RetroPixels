//go:build amd64 && (linux || windows)

#include "textflag.h"

// func jitCall(entry uintptr, a *jitArgs) uint64
//
// Loads the recompiled code's fixed registers from a (RDI: the table of
// writable pages), calls entry, and
// stores the budget left back into a. The code uses no stack beyond the
// return address.
TEXT ·jitCall(SB), NOSPLIT, $16-24
	MOVQ a+8(FP), SI
	MOVQ SI, 0(SP)
	MOVQ 0(SI), BX
	MOVQ 8(SI), R12
	MOVQ 16(SI), R13
	MOVQ 24(SI), R9
	MOVQ 32(SI), DI
	MOVQ entry+0(FP), AX
	CALL AX
	MOVQ 0(SP), SI
	MOVQ R9, 24(SI)
	MOVQ AX, ret+16(FP)
	RET

// func cpuHasFMA() bool: FMA3 and AVX, with the OS saving the AVX registers
TEXT ·cpuHasFMA(SB), NOSPLIT, $0-1
	MOVL $1, AX
	XORL CX, CX
	CPUID
	ANDL $0x18001000, CX
	CMPL CX, $0x18001000
	JNE no
	XORL CX, CX
	XGETBV
	ANDL $6, AX
	CMPL AX, $6
	JNE no
	MOVB $1, ret+0(FP)
	RET
no:
	MOVB $0, ret+0(FP)
	RET

// func cpuHasSSE41() bool: SSSE3 and SSE4.1
TEXT ·cpuHasSSE41(SB), NOSPLIT, $0-1
	MOVL $1, AX
	XORL CX, CX
	CPUID
	ANDL $0x80200, CX
	CMPL CX, $0x80200
	JNE no41
	MOVB $1, ret+0(FP)
	RET
no41:
	MOVB $0, ret+0(FP)
	RET

// func spuJitCall(entry uintptr, a *jitArgs) uint64
//
// jitCall for SPU code: single precision runs with denormals read and
// written as zero (as the SPU does), restoring MXCSR after.
TEXT ·spuJitCall(SB), NOSPLIT, $24-24
	MOVQ a+8(FP), SI
	MOVQ SI, 0(SP)
	STMXCSR 8(SP)
	MOVL $0x9fc0, 12(SP)
	LDMXCSR 12(SP)
	MOVQ 0(SI), BX
	MOVQ 8(SI), R12
	MOVQ 16(SI), R13
	MOVQ 24(SI), R9
	MOVQ 32(SI), DI
	MOVQ entry+0(FP), AX
	CALL AX
	LDMXCSR 8(SP)
	MOVQ 0(SP), SI
	MOVQ R9, 24(SI)
	MOVQ AX, ret+16(FP)
	RET
