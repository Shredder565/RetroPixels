//go:build amd64 && (linux || windows)

#include "textflag.h"

// func jitCall(entry uintptr, a *jitArgs) uint64
//
// Loads the recompiled code's fixed registers from a, calls entry, and
// stores the budget left back into a. The code uses no stack beyond the
// return address.
TEXT ·jitCall(SB), NOSPLIT, $16-24
	MOVQ a+8(FP), SI
	MOVQ SI, 0(SP)
	MOVQ 0(SI), BX
	MOVQ 8(SI), R12
	MOVQ 16(SI), R13
	MOVQ 24(SI), R9
	MOVQ entry+0(FP), AX
	CALL AX
	MOVQ 0(SP), SI
	MOVQ R9, 24(SI)
	MOVQ AX, ret+16(FP)
	RET
