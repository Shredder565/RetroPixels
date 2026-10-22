// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/MIOS.h"

#include <cstring>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/DSPEmulator.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/Wiimote.h"
#include "Core/Host.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace IOS::HLE::MIOS
{
namespace
{
// Handshake word between MIOS and the IPL: MIOS clears it, the IPL sets it to IPL_WAITING when
// it has initialised, and continues once MIOS clears it again.
constexpr u32 HANDSHAKE_ADDRESS = 0x30f8;
constexpr u32 IPL_WAITING = 0xdeadbeef;

// Low memory word the IPL reads to find the MIOS build.
constexpr u32 MIOS_INFO_ADDRESS = 0x3180;
constexpr u32 MIOS_INFO_VALUE = 0x09142001;

void EnterGameCubeMode(Core::System& system)
{
  system.SetIsWii(false);

  // MIOS wipes MEM2 before handing the console over.
  auto& memory = system.GetMemory();
  std::memset(memory.GetEXRAM(), 0, memory.GetExRamSizeReal());

  // MIOS resets the CPU and the drive interface. The drive is deliberately left alone here: a
  // full DI reset would also drop the audio streaming set-up that the System Menu made, and
  // games relying on streamed audio would then be silent.
  system.GetPowerPC().Reset();
  Wiimote::ResetAllWiimotes();

  // The DSP emulator was started for a Wii and has to be restarted for a GameCube.
  auto& dsp = system.GetDSP();
  dsp.Reinit(Config::Get(Config::MAIN_DSP_HLE));
  dsp.GetDSPEmulator()->Initialize(system.IsWii(), Config::Get(Config::MAIN_DSP_THREAD));

  system.GetSystemTimers().ChangePPCClock(SystemTimers::Mode::GC);
}

// Loads an optional symbol map for the IPL and re-applies HLE patches if one was found.
void RefreshIPLSymbols(Core::System& system, const Core::CPUThreadGuard& guard)
{
  auto& symbols = system.GetPowerPC().GetSymbolDB();
  bool changed = symbols.Clear();

  const std::string map_path = File::GetUserPath(D_MAPS_IDX) + "mios-ipl.map";
  if (symbols.LoadMap(guard, map_path))
  {
    ::HLE::Clear();
    ::HLE::PatchFunctions(system);
    changed = true;
  }

  if (changed)
    Host_PPCSymbolsChanged();
}

// Runs the IPL's start-up code until it signals readiness. The interpreter is used so the CPU
// can be stepped one instruction at a time.
void RunIPLUntilReady(Core::System& system)
{
  auto& cpu = system.GetPowerPC();
  auto& memory = system.GetMemory();

  const PowerPC::CoreMode previous_mode = cpu.GetMode();
  cpu.SetMode(PowerPC::CoreMode::Interpreter);

  auto& state = cpu.GetPPCState();
  state.msr.Hex = 0;
  state.pc = 0x3400;
  cpu.MSRUpdated();

  NOTICE_LOG_FMT(IOS, "MIOS: starting the IPL");

  while (memory.Read_U32(HANDSHAKE_ADDRESS) != IPL_WAITING)
    cpu.SingleStep();

  cpu.SetMode(previous_mode);
}
}  // namespace

bool Load(Core::System& system)
{
  ASSERT(Core::IsCPUThread());
  Core::CPUThreadGuard guard(system);

  auto& memory = system.GetMemory();
  memory.Write_U32(0, HANDSHAKE_ADDRESS);
  memory.Write_U32(MIOS_INFO_VALUE, MIOS_INFO_ADDRESS);

  EnterGameCubeMode(system);
  NOTICE_LOG_FMT(IOS, "MIOS: hardware switched to GameCube mode");

  RefreshIPLSymbols(system, guard);
  RunIPLUntilReady(system);

  // Release the IPL.
  memory.Write_U32(0, HANDSHAKE_ADDRESS);
  NOTICE_LOG_FMT(IOS, "MIOS: IPL is running");

  system.SetIsMIOS(true);
  system.GetDVDInterface().UpdateRunningGameMetadata();
  SConfig::OnTitleDirectlyBooted(guard);
  return true;
}
}  // namespace IOS::HLE::MIOS
