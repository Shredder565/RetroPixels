// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "../PPCRecompiler.h"
#include "../IML/IML.h"
#include "../IML/IMLRegisterAllocator.h"

// AArch64 backend (code is emitted with xbyak_aarch64).
//
// Register use inside generated code:
//   X19          PPCInterpreter_t* of the running core
//   X20          memory_base
//   X21          jump table
//   X0-X3, X16   scratch for the instruction being emitted; W0 holds the
//                guest address when generated code is left or a jump
//                table entry is entered
//   X17          scratch for addressing PPCInterpreter_t fields and guest memory
//   X4-X15, X22-X25   allocatable integer registers (colors 0-15)
//   V16-V31      allocatable vector registers (colors 0-15)
//   V0-V3        vector scratch
// X18 (platform register) and V8-V15 are never touched.
//
// Generated code runs inside the frame set up by the entry stub; SP stays
// the same while guest code runs, also when jumping between functions.
// Helpers called from generated code use the standard AAPCS64 convention.

IMLRegisterAllocatorParameters AArch64Backend_GetAllocatorParameters();

// creates the entry stub and the two exit stubs
void PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions();

// returns false if the function could not be compiled
bool PPCRecompiler_generateAArch64Code(PPCRecCompiledFunction* recFunc, IMLFunction& func, std::vector<std::pair<uint32, uint32>>& entryPointsOut);
