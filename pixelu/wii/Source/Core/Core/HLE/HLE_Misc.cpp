// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HLE/HLE_Misc.h"

#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/HW/CPU.h"
#include "Core/Host.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace HLE_Misc
{
namespace
{
// Number of times the code handler gets its icache flush before we stop
// helping. Some games only apply their patches once data has been loaded, so
// a handful of early frames is enough in practice.
constexpr u32 FLUSHED_RUNS = 5;

// Layout of the frame Gecko::RunCodeHandler pushes before entering the
// handler, as offsets from the saved stack pointer.
constexpr u32 FRAME_OLD_SP = 8;
constexpr u32 FRAME_OLD_PC = 12;
constexpr u32 FRAME_OLD_LR = 16;
constexpr u32 FRAME_OLD_CR = 20;
constexpr u32 FRAME_PAIRED_SINGLES = 24;
constexpr u32 SAVED_PS_REGISTERS = 14;
}  // namespace

// Stub for a function that should simply return to its caller; any result
// is whatever already sits in r3.
void UnimplementedFunction(const Core::CPUThreadGuard& guard)
{
  PowerPC::PowerPCState& state = guard.GetSystem().GetPPCState();
  state.npc = LR(state);
}

// A homebrew title asked to go back to its loader. We have no loader to go
// back to, so stop emulation instead.
void HBReload(const Core::CPUThreadGuard& guard)
{
  guard.GetSystem().GetCPU().Break();
  Host_Message(HostMessageID::WMUserStop);
}

// The code handler writes instructions without invalidating the icache. The
// installer's game-id word doubles as a run counter so the costly full reset
// only happens for the first few runs.
void GeckoCodeHandlerICacheFlush(const Core::CPUThreadGuard& guard)
{
  Core::System& system = guard.GetSystem();

  const u32 counter = PowerPC::MMU::HostRead<u32>(guard, Gecko::INSTALLER_BASE_ADDRESS);
  const u32 runs = counter - Gecko::MAGIC_GAMEID;
  if (runs == FLUSHED_RUNS)
    return;

  // Anything out of range (including a word that was never the magic value)
  // restarts the count.
  const u32 next = (runs < FLUSHED_RUNS ? counter : Gecko::MAGIC_GAMEID) + 1;
  PowerPC::MMU::HostWrite<u32>(guard, next, Gecko::INSTALLER_BASE_ADDRESS);

  system.GetPPCState().iCache.Reset(system.GetJitInterface());
}

// Entering the code handler is done by faking a branch-and-link from wherever
// the CPU was. This trampoline undoes that: it restores SP, PC, LR, CR and the
// first paired-single registers from the frame that was built on entry.
void GeckoReturnTrampoline(const Core::CPUThreadGuard& guard)
{
  PowerPC::PowerPCState& state = guard.GetSystem().GetPPCState();
  const u32 frame = state.gpr[1];
  const auto load32 = [&guard, frame](u32 offset) {
    return PowerPC::MMU::HostRead<u32>(guard, frame + offset);
  };

  state.gpr[1] = load32(FRAME_OLD_SP);
  state.npc = load32(FRAME_OLD_PC);
  LR(state) = load32(FRAME_OLD_LR);
  state.cr.Set(load32(FRAME_OLD_CR));

  u32 slot = frame + FRAME_PAIRED_SINGLES;
  for (u32 reg = 0; reg < SAVED_PS_REGISTERS; ++reg)
  {
    const u64 ps0 = PowerPC::MMU::HostRead<u64>(guard, slot);
    const u64 ps1 = PowerPC::MMU::HostRead<u64>(guard, slot + 8);
    state.ps[reg].SetBoth(ps0, ps1);
    slot += 16;
  }
}
}  // namespace HLE_Misc
