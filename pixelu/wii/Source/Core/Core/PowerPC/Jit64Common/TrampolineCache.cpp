// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64Common/TrampolineCache.h"

#include "Common/CommonTypes.h"
#include "Common/JitRegister.h"
#include "Common/MsgHandler.h"
#include "Common/x64Emitter.h"
#include "Core/PowerPC/Jit64Common/TrampolineInfo.h"

namespace
{
// A single trampoline never comes close to this; checking it up front keeps
// emission from running off the end of the region.
constexpr size_t TRAMPOLINE_HEADROOM = 1024;
}  // namespace

void TrampolineCache::ClearCodeSpace()
{
  Gen::X64CodeBlock::ClearCodeSpace();
}

const u8* TrampolineCache::GenerateTrampoline(const TrampolineInfo& info)
{
  return info.read ? GenerateReadTrampoline(info) : GenerateWriteTrampoline(info);
}

const u8* TrampolineCache::GenerateReadTrampoline(const TrampolineInfo& info)
{
  if (GetSpaceLeft() < TRAMPOLINE_HEADROOM)
    PanicAlertFmt("Trampoline cache full");

  const u8* const entry = GetCodePtr();
  const int bits = info.accessSize * 8;
  SafeLoadToReg(info.op_reg, info.op_arg, bits, info.offset, info.registersInUse, info.signExtend,
                info.flags | SAFE_LOADSTORE_FORCE_SLOW_ACCESS);
  JMP(info.start + info.len);
  Common::JitRegister::Register(entry, GetCodePtr(), "JIT_ReadTrampoline_{:x}", info.pc);
  return entry;
}

const u8* TrampolineCache::GenerateWriteTrampoline(const TrampolineInfo& info)
{
  if (GetSpaceLeft() < TRAMPOLINE_HEADROOM)
    PanicAlertFmt("Trampoline cache full");

  // Gather-pipe stores take the generic slow path here too; it already does
  // the burst check.
  const u8* const entry = GetCodePtr();
  const int bits = info.accessSize * 8;
  SafeWriteRegToReg(info.op_arg, info.op_reg, bits, info.offset, info.registersInUse,
                    info.flags | SAFE_LOADSTORE_FORCE_SLOW_ACCESS);
  JMP(info.start + info.len);
  Common::JitRegister::Register(entry, GetCodePtr(), "JIT_WriteTrampoline_{:x}", info.pc);
  return entry;
}
