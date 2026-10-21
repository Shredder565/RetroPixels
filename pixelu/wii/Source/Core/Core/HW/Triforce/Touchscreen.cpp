// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/Triforce/Touchscreen.h"

#include <array>
#include <numeric>

#include "Common/BitUtils.h"
#include "Common/Logging/Log.h"
#include "Core/HW/GCPad.h"
#include "InputCommon/GCPadStatus.h"

namespace Triforce
{

void Touchscreen::Update()
{
  // The panel takes no commands; anything the game sends is dropped.
  const std::size_t stray = GetRxByteSpan().size();
  if (stray != 0)
  {
    WARN_LOG_FMT(SERIALINTERFACE_AMBB, "Unexpected write of {} bytes to touchscreen.", stray);
    ConsumeRxBytes(stray);
  }

  const GCPadStatus pad = Pad::GetStatus(0);
  const u16 x = Common::ExpandValue<u16>(u16{pad.substickX}, 4);
  const u16 y = Common::ExpandValue<u16>(u16{pad.substickY}, 4);

  std::array<u8, 10> packet{
      0x55,
      0x54,
      0xFF,
      static_cast<u8>(x & 0xFF),
      static_cast<u8>(x >> 8),
      static_cast<u8>(y & 0xFF),
      static_cast<u8>(y >> 8),
      pad.triggerRight,
      0x00,
      0x00,
  };
  const u32 sum = std::accumulate(packet.begin(), packet.begin() + 9, u32{0});
  packet[9] = static_cast<u8>(0xAA + sum);

  WriteTxBytes(packet);
}

}  // namespace Triforce
