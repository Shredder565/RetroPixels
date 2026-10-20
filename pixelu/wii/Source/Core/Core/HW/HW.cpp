// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/HW.h"

#include "Common/ChunkFile.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/HSP/HSP.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/MemoryInterface.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/WII_IPC.h"
#include "Core/IOS/IOS.h"
#include "Core/State.h"
#include "Core/System.h"

namespace HW
{
namespace
{
// Saves or loads one device, followed by a marker carrying its name.
template <typename Device>
void DeviceState(PointerWrap& p, Device& device, const char* marker)
{
  device.DoState(p);
  p.DoMarker(marker);
}

// The scheduler and savestate service come first; devices schedule events
// and register savestate users while they initialise.
void BringUpServices(Core::System& system)
{
  system.GetCoreTiming().Init();
  system.GetSystemTimers().PreInit();
  State::Init(system);
}

// Device start-up. The order carries dependencies: EXI (SRAM) before memory,
// memory before the address spaces, and the PowerPC core after the devices
// whose timing it reads.
void BringUpDevices(Core::System& system, const Sram* override_sram)
{
  system.GetAudioInterface().Init();
  system.GetVideoInterface().Init();
  system.GetSerialInterface().Init();
  system.GetProcessorInterface().Init();
  system.GetExpansionInterface().Init(override_sram);
  system.GetHSP().Init();

  system.GetMemory().Init();
  AddressSpace::Init();
  system.GetMemoryInterface().Init();

  system.GetDSP().Init(Config::Get(Config::MAIN_DSP_HLE));
  system.GetDVDInterface().Init();
  system.GetGPFifo().Init();
  system.GetCPU().Init(Config::Get(Config::MAIN_CPU_CORE));
  system.GetSystemTimers().Init();
}

// Teardown runs roughly in reverse.
void TearDownDevices(Core::System& system)
{
  system.GetSystemTimers().Shutdown();
  system.GetCPU().Shutdown();
  system.GetDVDInterface().Shutdown();
  system.GetDSP().Shutdown();

  system.GetMemoryInterface().Shutdown();
  AddressSpace::Shutdown();
  system.GetMemory().Shutdown();

  system.GetHSP().Shutdown();
  system.GetExpansionInterface().Shutdown();
  system.GetSerialInterface().Shutdown();
  system.GetAudioInterface().Shutdown();
}
}  // namespace

void Init(Core::System& system, const Sram* override_sram)
{
  BringUpServices(system);
  BringUpDevices(system, override_sram);

  if (system.IsWii())
  {
    system.GetWiiIPC().Init();
    // Needs main memory.
    IOS::HLE::Init(system);
  }

  // Last, once every device that owns registers exists.
  system.GetMemory().InitMMIO(system);
}

void Shutdown(Core::System& system)
{
  // IOS also runs on GameCube titles (as MIOS), so it is always shut down,
  // before the memory it uses goes away.
  IOS::HLE::Shutdown(system);
  system.GetWiiIPC().Shutdown();

  TearDownDevices(system);

  State::Shutdown();
  system.GetCoreTiming().Shutdown();
}

void DoState(Core::System& system, PointerWrap& p)
{
  // The section order and marker names are part of the savestate format.
  DeviceState(p, system.GetMemory(), "Memory");
  DeviceState(p, system.GetMemoryInterface(), "MemoryInterface");
  DeviceState(p, system.GetVideoInterface(), "VideoInterface");
  DeviceState(p, system.GetSerialInterface(), "SerialInterface");
  DeviceState(p, system.GetProcessorInterface(), "ProcessorInterface");
  DeviceState(p, system.GetDSP(), "DSP");
  DeviceState(p, system.GetDVDInterface(), "DVDInterface");
  DeviceState(p, system.GetGPFifo(), "GPFifo");
  DeviceState(p, system.GetExpansionInterface(), "ExpansionInterface");
  DeviceState(p, system.GetAudioInterface(), "AudioInterface");
  DeviceState(p, system.GetHSP(), "HSP");

  if (system.IsWii())
  {
    DeviceState(p, system.GetWiiIPC(), "IOS");
    DeviceState(p, *system.GetIOS(), "IOS::HLE");
  }

  p.DoMarker("WIIHW");
}
}  // namespace HW
