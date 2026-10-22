// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/Triforce/FZeroAX.h"

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
constexpr std::size_t WHEEL_REQUEST_SIZE = 4;

// Pad button -> switch bit, for one switch byte.
struct ButtonBit
{
  u16 button;
  u8 bit;
};

void MapButtons(u16 buttons, u8& target, std::span<const ButtonBit> map)
{
  for (const ButtonBit& entry : map)
  {
    if ((buttons & entry.button) != 0)
      target |= entry.bit;
  }
}

constexpr std::array<ButtonBit, 6> PLAYER1_BUTTONS{{
    {PAD_BUTTON_START, 0x80},
    {PAD_BUTTON_RIGHT, 0x20},
    {PAD_BUTTON_LEFT, 0x10},
    {PAD_BUTTON_UP, 0x08},
    {PAD_BUTTON_DOWN, 0x04},
    {PAD_BUTTON_A, 0x02},
}};

constexpr std::array<ButtonBit, 2> PLAYER2_BUTTONS{{
    {PAD_BUTTON_X, 0x20},
    {PAD_BUTTON_Y, 0x10},
}};

constexpr std::array<std::pair<u8, std::string_view>, 6> LAMPS{{
    {0x80, "START BUTTON"},
    {0x20, "VIEW CHANGE 1"},
    {0x10, "VIEW CHANGE 2"},
    {0x08, "VIEW CHANGE 3"},
    {0x04, "VIEW CHANGE 4"},
    {0x40, "BOOST"},
}};
}  // namespace

void FZeroAXSteeringWheel::Update()
{
  const std::span<const u8> rx = GetRxByteSpan();
  std::size_t pos = 0;

  while (rx.size() - pos >= WHEEL_REQUEST_SIZE)
  {
    const std::span<const u8> request = rx.subspan(pos, WHEEL_REQUEST_SIZE);
    const u8 check = static_cast<u8>(request[0] ^ request[1] ^ request[2] ^ request[3] ^ 0x80);
    if (check != 0)
    {
      WARN_LOG_FMT(SERIALINTERFACE_AMBB, "SteeringWheel: Bad checksum!");
      ++pos;
      continue;
    }
    HandleRequest(request);
    pos += WHEEL_REQUEST_SIZE;
  }

  ConsumeRxBytes(pos);
}

void FZeroAXSteeringWheel::HandleRequest(std::span<const u8> request)
{
  const u8 command = static_cast<u8>(request[0] ^ 0x80);
  DEBUG_LOG_FMT(SERIALINTERFACE_AMBB, "SteeringWheel: Request: {:02x}", command);

  switch (command)
  {
  case 0x04:
  {
    const u8 raw = static_cast<u8>(static_cast<u8>(request[1] << 7) | request[2]);
    m_servo = static_cast<s8>(static_cast<u8>(0x80 - raw));
    DEBUG_LOG_FMT(SERIALINTERFACE_AMBB, "SteeringWheel: servo_position: {}", m_servo);
    break;
  }
  case 0x07:
    m_init_state = 2;
    break;
  case 0x7F:
    m_init_state = 1;
    break;
  default:
    break;
  }

  static constexpr std::array<u8, 4> ack{};
  WriteTxBytes(ack);
}

bool FZeroAXSteeringWheel::IsInitializing() const
{
  return m_init_state == 1;
}

void FZeroAXSteeringWheel::DoState(PointerWrap& p)
{
  // Only the wheel's own state; queued serial bytes are not part of it.
  p.Do(m_init_state);
  p.Do(m_servo);
}

void FZeroAXCommon_IOAdapter::Update()
{
  IOPorts& ports = *GetIOPorts();
  ports.GetStatusSwitches()[0] &= static_cast<u8>(0xDF);

  const GCPadStatus pad = Pad::GetStatus(0);
  MapButtons(pad.button, ports.GetSwitchInputs(0)[0], PLAYER1_BUTTONS);
  MapButtons(pad.button, ports.GetSwitchInputs(1)[0], PLAYER2_BUTTONS);

  const std::span<u16> analog = ports.GetAnalogInputs();
  if (m_wheel->IsInitializing())
    analog[0] = static_cast<u16>(int{m_wheel->GetServoPosition()} * 512 + 0x8000);
  else
    analog[0] = Common::ExpandValue<u16>(pad.stickX, 8);
  analog[1] = Common::ExpandValue<u16>(pad.stickY, 8);
  analog[4] = Common::ExpandValue<u16>(pad.triggerRight, 8);
  analog[5] = Common::ExpandValue<u16>(pad.triggerLeft, 8);
  analog[6] = IOPorts::NEUTRAL_ANALOG_VALUE;
}

void FZeroAXCommon_IOAdapter::HandleGenericOutputsChanged(std::span<const u8> bits_set,
                                                          std::span<const u8> bits_cleared)
{
  const u8 changed = static_cast<u8>(bits_set[0] | bits_cleared[0]);
  for (const auto& [bit, name] : LAMPS)
  {
    if ((changed & bit) == 0)
      continue;
    INFO_LOG_FMT(SERIALINTERFACE_JVSIO, "JVS-IO: {}: {}", name,
                 (bits_set[0] & bit) != 0 ? "ON" : "OFF");
  }
}

void FZeroAXDeluxe_IOAdapter::Update()
{
  IOPorts& ports = *GetIOPorts();
  const std::span<const u8> outputs = ports.GetGenericOutputs();
  const u32 seat = ((u32{outputs[1]} << 8) | outputs[2]) >> 2;

  if (seat == 0x70)
  {
    ++m_delay;
    if (m_delay % 10 == 0)
      m_rx_reply = 0xFB;
  }
  else if (seat == 0xF0)
  {
    m_rx_reply = 0xF0;
  }

  const std::span<u8> p1 = ports.GetSwitchInputs(0);
  const std::span<u8> p2 = ports.GetSwitchInputs(1);
  p1[0] |= 0x01;
  p1[1] = static_cast<u8>(m_rx_reply & 0xF0);
  p2[1] = static_cast<u8>(m_rx_reply << 4);
}

void FZeroAXDeluxe_IOAdapter::DoState(PointerWrap& p)
{
  p.Do(m_delay);
  p.Do(m_rx_reply);
}

void FZeroAXMonster_IOAdapter::Update()
{
  GetIOPorts()->GetSwitchInputs(1)[0] |= 0x02;
}

}  // namespace Triforce
