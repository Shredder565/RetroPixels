// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef HAS_LIBMGBA

#include <memory>

#include "Common/CommonTypes.h"
#include "Core/HW/SI/SI_Device.h"

namespace HW::GBA
{
class Core;
}  // namespace HW::GBA

class GBAHostInterface;

namespace SerialInterface
{
// A GBA on the link cable emulated in-process by mGBA (see HW/GBACore).
class CSIDevice_GBAEmu final : public ISIDevice
{
public:
  CSIDevice_GBAEmu(Core::System& system, SIDevices device, int device_number);
  ~CSIDevice_GBAEmu() override;

  int RunBuffer(u8* buffer, int request_length) override;
  int TransferInterval() override;
  DataResponse GetData(u32& hi, u32& low) override;
  void SendCommand(u32 command, u8 poll) override;
  void DoState(PointerWrap& p) override;
  void OnEvent(u64 userdata, s64 cycles_late) override;

private:
  // Saved in savestates as an int; the values must stay.
  enum class Phase
  {
    Idle = 0,
    InFlight = 1,
    AwaitReply = 2,
  };

  Phase m_phase = Phase::Idle;
  EBufferCommands m_pending_command{};
  u64 m_sent_at = 0;
  // GBA key bits from the last poll.
  u16 m_keys = 0;

  std::shared_ptr<HW::GBA::Core> m_core;
  std::shared_ptr<GBAHostInterface> m_gbahost;
};
}  // namespace SerialInterface
#endif  // HAS_LIBMGBA
