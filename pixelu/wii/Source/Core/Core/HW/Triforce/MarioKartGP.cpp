// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/Triforce/MarioKartGP.h"

#include <array>
#include <string_view>
#include <utility>

#include "Common/BitUtils.h"
#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Core/HW/GCPad.h"
#include "InputCommon/GCPadStatus.h"

namespace Triforce
{

namespace
{
constexpr std::size_t WHEEL_REQUEST_SIZE = 10;
constexpr u32 WHEEL_REQUEST_MAGIC = 0xFFFFFF01;

constexpr std::array<std::pair<u8, std::string_view>, 2> LAMPS{{
    {0x04, "ITEM BUTTON"},
    {0x08, "CANCEL BUTTON"},
}};

u16 ReadBE16(std::span<const u8> bytes, std::size_t at)
{
  return static_cast<u16>((bytes[at] << 8) | bytes[at + 1]);
}
}  // namespace

void MarioKartGPSteeringWheel::Update()
{
  const std::span<const u8> rx = GetRxByteSpan();
  std::size_t used = 0;
  while (rx.size() - used >= WHEEL_REQUEST_SIZE)
  {
    HandleRequest(rx.subspan(used, WHEEL_REQUEST_SIZE));
    used += WHEEL_REQUEST_SIZE;
  }
  ConsumeRxBytes(used);
}

void MarioKartGPSteeringWheel::HandleRequest(std::span<const u8> request)
{
  const u32 magic = (u32{request[0]} << 24) | (u32{request[1]} << 16) | (u32{request[2]} << 8) |
                    u32{request[3]};
  if (magic != WHEEL_REQUEST_MAGIC)
  {
    WARN_LOG_FMT(SERIALINTERFACE_AMBB, "SteeringWheel: Unknown command: {:08x}", magic);
    return;
  }

  DEBUG_LOG_FMT(SERIALINTERFACE_AMBB, "SteeringWheel: FFB: {:04x} {:04x} {:04x}",
                ReadBE16(request, 4), ReadBE16(request, 6), ReadBE16(request, 8));

  std::string_view reply;
  switch (m_init_state)
  {
  case 0:
    reply = "E00";
    m_init_state = 1;
    break;
  case 1:
    reply = "C06";
    m_init_state = 2;
    break;
  default:
    reply = "C01";
    m_init_state = 1;
    break;
  }
  for (const char c : reply)
    WriteTxByte(static_cast<u8>(c));
}

void MarioKartGPSteeringWheel::Reset()
{
  INFO_LOG_FMT(SERIALINTERFACE_AMBB, "SteeringWheel: Reset");
  m_init_state = 0;
}

void MarioKartGPSteeringWheel::DoState(PointerWrap& p)
{
  // Only the handshake position; queued serial bytes are not part of it.
  p.Do(m_init_state);
}

void MarioKartGPCommon_IOAdapter::Update()
{
  IOPorts& ports = *GetIOPorts();
  const GCPadStatus pad = Pad::GetStatus(0);

  const std::span<u8> p1 = ports.GetSwitchInputs(0);
  if ((pad.button & PAD_BUTTON_START) != 0)
    p1[0] |= 0x80;
  if ((pad.button & PAD_BUTTON_A) != 0)
    p1[1] |= 0x20;
  if ((pad.button & PAD_BUTTON_B) != 0)
    p1[1] |= 0x02;

  const std::span<u16> analog = ports.GetAnalogInputs();
  analog[0] = Common::ExpandValue<u16>(pad.stickX, 8);
  analog[1] = Common::ExpandValue<u16>(pad.triggerRight, 8);
  analog[2] = Common::ExpandValue<u16>(pad.triggerLeft, 8);
}

void MarioKartGPCommon_IOAdapter::HandleGenericOutputsChanged(std::span<const u8> bits_set,
                                                              std::span<const u8> bits_cleared)
{
  if ((bits_set[0] & 0x80) != 0)
    m_wheel->Reset();

  const u8 changed = static_cast<u8>(bits_set[0] | bits_cleared[0]);
  for (const auto& [bit, name] : LAMPS)
  {
    if ((changed & bit) == 0)
      continue;
    INFO_LOG_FMT(SERIALINTERFACE_JVSIO, "JVS-IO: {}: {}", name,
                 (bits_set[0] & bit) != 0 ? "ON" : "OFF");
  }
}

}  // namespace Triforce
