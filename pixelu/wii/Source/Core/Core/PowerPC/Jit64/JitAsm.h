// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/Jit64Common/Jit64AsmCommon.h"

namespace Gen
{
class X64CodeBlock;
}

// Owns the code that is not tied to a guest block: the entry and dispatch
// loop that blocks exit into, and the shared helper routines. It is
// generated at startup and after every cache clear, then write-protected.
class Jit64AsmRoutineManager : public CommonAsmRoutines
{
public:
  // Room for the dispatcher and every shared routine.
  static constexpr size_t CODE_SIZE = 16384;

  explicit Jit64AsmRoutineManager(Jit64& jit);

  void Init();
  void Regenerate();

  // Emits a reset of RSP to the value saved when the JIT was entered.
  void ResetStack(Gen::X64CodeBlock& emitter);

private:
  void Generate();
  void GenerateCommon();

  // Looks up the block for (pc, feature_flags) and jumps to it; falls
  // through when there is none.
  void EmitBlockLookup();
};
