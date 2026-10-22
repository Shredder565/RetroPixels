// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/x64Emitter.h"

// Everything needed to replace a faulting fastmem access with a jump to a
// slow-path trampoline. One of these exists per fastmem access in the code
// cache, which can be tens of thousands, so the struct is kept small.
struct TrampolineInfo final
{
  // First byte of the fastmem access; the patch JMP is written here.
  u8* start = nullptr;

  // Byte length of the access, so start + len is where execution resumes.
  u16 len = 0;

  // SafeLoadStoreFlags the access was emitted with.
  u8 flags = 0;

  // Access width in bytes.
  u8 accessSize : 4 = 0;

  // Load (true) or store (false).
  bool read : 1 = false;

  // Loads only: whether the value is sign-extended.
  bool signExtend : 1 = false;

  // The offset was folded into the address register before the access and
  // must be subtracted again by the handler.
  bool offsetAddedToAddress : 1 = false;

  // Guest address of the instruction being emulated.
  u32 pc = 0;

  // Host registers live across the access; the slow path preserves them.
  BitSet32 registersInUse{};

  // Stores without MOVBE: the register that was byte-swapped in place.
  Gen::X64Reg nonAtomicSwapStoreSrc{};

  // Operands of the access, replayed by the trampoline.
  s32 offset = 0;
  Gen::X64Reg op_reg{};
  Gen::OpArg op_arg{};
};
