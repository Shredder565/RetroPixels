// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The emulator's side of DSP/DSPHost.h: memory, interrupts and code-load hooks
// for the LLE DSP.

#include "Core/DSP/DSPHost.h"

#include <string>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/DSP/DSPAnalyzer.h"
#include "Core/DSP/DSPCodeUtil.h"
#include "Core/DSP/DSPCore.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DSPLLE/DSPSymbols.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace DSP::Host
{
u8 ReadHostMemory(u32 addr)
{
  return Core::System::GetInstance().GetDSP().ReadARAM(addr);
}

void WriteHostMemory(u8 value, u32 addr)
{
  Core::System::GetInstance().GetDSP().WriteARAM(value, addr);
}

void DMAToDSP(u16* dst, u32 addr, u32 size)
{
  Core::System::GetInstance().GetMemory().CopyFromEmuSwapped(dst, addr, size);
}

void DMAFromDSP(const u16* src, u32 addr, u32 size)
{
  Core::System::GetInstance().GetMemory().CopyToEmuSwapped(addr, src, size);
}

void OSD_AddMessage(std::string str, u32 ms)
{
  OSD::AddMessage(std::move(str), ms);
}

bool OnThread()
{
  return Config::Get(Config::MAIN_DSP_THREAD);
}

bool IsWiiHost()
{
  return Core::System::GetInstance().IsWii();
}

void InterruptRequest()
{
  Core::System::GetInstance().GetDSP().GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP);
}

void CodeLoaded(DSPCore& dsp, u32 addr, size_t size)
{
  const u8* const code = Core::System::GetInstance().GetMemory().GetPointerForRange(addr, size);
  CodeLoaded(dsp, code, size);
}

void CodeLoaded(DSPCore& dsp, const u8* ptr, size_t size)
{
  SDSP& state = dsp.DSPState();

  // The checksum identifies the microcode (for HLE matching and for dumps).
  const u32 crc = Common::HashEctor(ptr, size);
  state.SetIRAMCRC(crc);
  if (Config::Get(Config::MAIN_DUMP_UCODE))
    DumpDSPCode(ptr, size, crc);
  NOTICE_LOG_FMT(DSPLLE, "IRAM code checksum: {:08x}", crc);

  // Refresh the debugger listing of IRAM and IROM.
  Symbols::Clear();
  Symbols::AutoDisassembly(state, 0x0000, 0x1000);
  Symbols::AutoDisassembly(state, 0x8000, 0x9000);

  // Drop compiled blocks of the old code and re-analyze.
  dsp.ClearIRAM();
  state.GetAnalyzer().Analyze(state);
}
}  // namespace DSP::Host
