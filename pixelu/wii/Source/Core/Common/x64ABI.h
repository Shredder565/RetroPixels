// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/BitSet.h"
#include "Common/x64Reg.h"

// Calling-convention facts the JITs need when they call into C++ code.
//
// Register masks are BitSet32 values: bits 0-15 are the general purpose registers RAX..R15, bits
// 16-31 are XMM0..XMM15 (XMMn is bit 16 + n).
//
// Microsoft x64 (Windows): arguments in RCX, RDX, R8, R9; the caller reserves 32 bytes of shadow
// space above the return address; RAX, RCX, RDX, R8-R11 and XMM0-XMM5 are volatile.
//
// System V AMD64 (everything else): arguments in RDI, RSI, RDX, RCX, R8, R9; no shadow space;
// RAX, RCX, RDX, RSI, RDI, R8-R11 and every XMM register are volatile.
//
// Integer results come back in RAX.

#define ABI_ALL_GPRS BitSet32(0x0000ffff)
#define ABI_ALL_FPRS BitSet32(0xffff0000)

#ifdef _WIN32

#define ABI_PARAM1 RCX
#define ABI_PARAM2 RDX
#define ABI_PARAM3 R8
#define ABI_PARAM4 R9

#define ABI_ALL_CALLER_SAVED                                                                       \
  (BitSet32{RAX, RCX, RDX, R8, R9, R10, R11, 16 + XMM0, 16 + XMM1, 16 + XMM2, 16 + XMM3,           \
            16 + XMM4, 16 + XMM5})

#else

#define ABI_PARAM1 RDI
#define ABI_PARAM2 RSI
#define ABI_PARAM3 RDX
#define ABI_PARAM4 RCX
#define ABI_PARAM5 R8
#define ABI_PARAM6 R9

#define ABI_ALL_CALLER_SAVED (BitSet32{RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11} | ABI_ALL_FPRS)

#endif

#define ABI_ALL_CALLEE_SAVED (~ABI_ALL_CALLER_SAVED)

#define ABI_RETURN RAX
