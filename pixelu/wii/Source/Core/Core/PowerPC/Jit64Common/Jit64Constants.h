// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>

#include "Common/x64Reg.h"

// Host registers with a fixed role in Jit64-generated code.
//
// The two scratch registers are never handed out by the register caches, so
// any emitted sequence may clobber them freely.
constexpr Gen::X64Reg RSCRATCH = Gen::RAX;
constexpr Gen::X64Reg RSCRATCH2 = Gen::RDX;

// A third scratch register. Unlike the two above it is also allocatable, so
// it must be flushed or locked before generated code uses it.
constexpr Gen::X64Reg RSCRATCH_EXTRA = Gen::RCX;

// Base of the emulated address space (fastmem arena or logical view).
constexpr Gen::X64Reg RMEM = Gen::RBX;

// Points 0x80 bytes into the PowerPC state, so that the first 256 bytes of it
// can be addressed with a signed 8-bit displacement.
constexpr Gen::X64Reg RPPCSTATE = Gen::RBP;

// Size of the near code region.
constexpr size_t CODE_SIZE = 1024 * 1024 * 128;
