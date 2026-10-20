// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PowerPC.h"

// Reason bits a program exception leaves in SRR1 (PowerPC bits 11 to 14).
enum class ProgramExceptionCause : u32
{
  FloatingPoint = 0x00100000,
  IllegalInstruction = 0x00080000,
  PrivilegedInstruction = 0x00040000,
  Trap = 0x00020000,
};

// An access that the 750CL cannot do with the given alignment. DAR receives the address.
inline void GenerateAlignmentException(PowerPC::PowerPCState& ppc_state, u32 address)
{
  ppc_state.spr[SPR_DAR] = address;
  ppc_state.Exceptions |= EXCEPTION_ALIGNMENT;
}

// A data access that failed for a reason the MMU code does not raise itself (eciwx/ecowx).
inline void GenerateDSIException(PowerPC::PowerPCState& ppc_state, u32 address)
{
  ppc_state.spr[SPR_DAR] = address;
  ppc_state.Exceptions |= EXCEPTION_DSI;
}

// SRR1 is overwritten with the reason; PowerPCManager::CheckExceptions merges MSR bits into it.
inline void GenerateProgramException(PowerPC::PowerPCState& ppc_state, ProgramExceptionCause cause)
{
  ppc_state.spr[SPR_SRR1] = static_cast<u32>(cause);
  ppc_state.Exceptions |= EXCEPTION_PROGRAM;
}
