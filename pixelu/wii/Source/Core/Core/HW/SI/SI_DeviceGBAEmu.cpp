// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef HAS_LIBMGBA

#include "Core/HW/SI/SI_DeviceGBAEmu.h"

#include <array>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GBACore.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_DeviceGCController.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Host.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"

namespace SerialInterface
{
namespace
{
// The emulated GBA is run forward at least once per millisecond of GameCube
// time, so it stays close to the console even without link traffic.
s64 SyncPeriod(const SystemTimers::SystemTimersManager& timers)
{
  return timers.GetTicksPerSecond() / 1000;
}

// Pad buttons in the order of the GBA's KEYINPUT bits (A, B, Select, Start,
// Right, Left, Up, Down, R, L). Select is mapped to Z.
constexpr std::array<u16, 10> GBA_KEY_SOURCES = {
    PAD_BUTTON_A,     PAD_BUTTON_B,    PAD_TRIGGER_Z, PAD_BUTTON_START, PAD_BUTTON_RIGHT,
    PAD_BUTTON_LEFT,  PAD_BUTTON_UP,   PAD_BUTTON_DOWN, PAD_TRIGGER_R,  PAD_TRIGGER_L,
};
}  // namespace

CSIDevice_GBAEmu::CSIDevice_GBAEmu(Core::System& system, SIDevices device, int device_number)
    : ISIDevice(system, device, device_number)
{
  m_core = std::make_shared<HW::GBA::Core>(system, m_device_number);
  m_core->Start(system.GetCoreTiming().GetTicks());
  m_gbahost = Host_CreateGBAHost(m_core);
  m_core->SetHost(m_gbahost);
  system.GetSerialInterface().ScheduleEvent(m_device_number, SyncPeriod(system.GetSystemTimers()));
}

CSIDevice_GBAEmu::~CSIDevice_GBAEmu()
{
  m_system.GetSerialInterface().RemoveEvent(m_device_number);
  m_core->Stop();
  m_gbahost.reset();
  m_core.reset();
}

int CSIDevice_GBAEmu::RunBuffer(u8* buffer, int)
{
  auto& si = m_system.GetSerialInterface();
  const u64 now = m_system.GetCoreTiming().GetTicks();

  if (m_phase == Phase::Idle)
  {
    m_pending_command = static_cast<EBufferCommands>(buffer[0]);
    m_sent_at = now;
    const int transfer_time = TransferInterval();
    m_core->SendJoybusCommand(m_sent_at, transfer_time, buffer, m_keys);

    // Restart this port's sync timer after the transfer, and bring every
    // other emulated GBA up to the same point in time right away.
    si.RemoveEvent(m_device_number);
    si.ScheduleEvent(m_device_number, transfer_time + SyncPeriod(m_system.GetSystemTimers()));
    for (int port = 0; port < MAX_SI_CHANNELS; ++port)
    {
      if (port == m_device_number || si.GetDeviceType(port) != GetDeviceType())
        continue;
      si.RemoveEvent(port);
      si.ScheduleEvent(port, 0, static_cast<u64>(transfer_time));
    }
    m_phase = Phase::InFlight;
  }

  if (m_phase == Phase::InFlight)
  {
    if (TransferInterval() > static_cast<int>(now - m_sent_at))
      return 0;
    m_phase = Phase::AwaitReply;
  }

  m_phase = Phase::Idle;
  const int length = m_core->GetJoybusResponse(buffer);
  if (length == 0)
    return -1;
  DEBUG_LOG_FMT(SERIALINTERFACE, "GBA {}: reply to {:#04x}, {} bytes", m_device_number,
                static_cast<u8>(m_pending_command), length);
  return length;
}

int CSIDevice_GBAEmu::TransferInterval()
{
  return SIDevice_GetGBATransferTime(m_system.GetSystemTimers(), m_pending_command);
}

DataResponse CSIDevice_GBAEmu::GetData(u32&, u32&)
{
  GCPadStatus pad{};
  if (!NetPlay::IsNetPlayRunning())
    pad = Pad::GetGBAStatus(m_device_number);
  CSIDevice_GCController::HandleMoviePadStatus(m_system.GetMovie(), m_device_number, &pad);

  u16 keys = 0;
  for (std::size_t bit = 0; bit < GBA_KEY_SOURCES.size(); ++bit)
  {
    if ((pad.button & GBA_KEY_SOURCES[bit]) != 0)
      keys = static_cast<u16>(keys | (1u << bit));
  }
  m_keys = keys;

  // X carries the reset request (so it also works through NetPlay/movies).
  if ((pad.button & PAD_BUTTON_X) != 0)
    m_core->Reset();

  // The GBA is never polled; it only talks through buffer transfers.
  return DataResponse::NoData;
}

void CSIDevice_GBAEmu::SendCommand(u32, u8)
{
}

void CSIDevice_GBAEmu::DoState(PointerWrap& p)
{
  p.Do(m_phase);
  p.Do(m_pending_command);
  p.Do(m_sent_at);
  p.Do(m_keys);
  m_core->DoState(p);
}

void CSIDevice_GBAEmu::OnEvent(u64 userdata, s64)
{
  // `userdata` is how far past "now" the GBA should run (non-zero when a
  // transfer on another port triggered this sync).
  m_core->SyncJoybus(m_system.GetCoreTiming().GetTicks() + userdata, m_keys);
  const u64 next = userdata + static_cast<u64>(SyncPeriod(m_system.GetSystemTimers()));
  m_system.GetSerialInterface().ScheduleEvent(m_device_number, static_cast<s64>(next));
}
}  // namespace SerialInterface
#endif  // HAS_LIBMGBA
