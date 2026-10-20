// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/SI/SI_DeviceGCSteeringWheel.h"

#include <algorithm>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/GCPad.h"

namespace SerialInterface
{
namespace
{
// Mode the wheel's games select; other modes behave like a normal pad.
constexpr u8 WHEEL_MODE = 6;
// Set in the high word when the pedal unit is plugged into the wheel.
constexpr u32 PEDALS_CONNECTED = 0x800;

// Force feedback command, in the top 7 bits of the second parameter.
constexpr u32 FORCE_MOTOR_OFF = 0x02;
constexpr u32 FORCE_MOTOR_ON = 0x03;

// The pedals have no mapping of their own: the upper half of the main stick
// or the lower half of the C-stick is the accelerator, and the opposite
// halves are the brake, so both can be pressed at once. Half a stick is
// scaled up to the full 0..255 pedal range.
u32 PedalValue(int main_half, int c_half)
{
  return static_cast<u32>(std::clamp(std::max(main_half, c_half) * 2, 0, 0xff));
}
}  // namespace

CSIDevice_GCSteeringWheel::CSIDevice_GCSteeringWheel(Core::System& system, SIDevices device,
                                                     int device_number)
    : CSIDevice_GCController(system, device, device_number)
{
}

int CSIDevice_GCSteeringWheel::RunBuffer(u8* buffer, int request_length)
{
  const auto command = static_cast<EBufferCommands>(buffer[0]);
  if (command != EBufferCommands::CMD_STATUS && command != EBufferCommands::CMD_RESET)
    return CSIDevice_GCController::RunBuffer(buffer, request_length);

  ISIDevice::RunBuffer(buffer, request_length);
  return CreateStatusResponse(SI_GC_STEERING, buffer);
}

DataResponse CSIDevice_GCSteeringWheel::GetData(u32& hi, u32& low)
{
  if (m_mode != WHEEL_MODE)
    return CSIDevice_GCController::GetData(hi, low);

  const GCPadStatus pad = GetPadStatus();
  const int stick_y = pad.stickY;
  const int c_y = pad.substickY;
  const int main_center = GCPadStatus::MAIN_STICK_CENTER_Y;
  const int c_center = GCPadStatus::C_STICK_CENTER_Y;

  const u32 buttons = static_cast<u16>(pad.button | PAD_USE_ORIGIN);
  hi = (buttons << 16) | PEDALS_CONNECTED | u32{pad.stickX};

  const u32 accelerator = PedalValue(stick_y - main_center, c_center - c_y);
  const u32 brake = PedalValue(c_y - c_center, main_center - stick_y);
  low = (accelerator << 24) | (brake << 16) | (u32{pad.triggerLeft} << 8) | pad.triggerRight;

  HandleButtonCombos(pad);
  return DataResponse::Success;
}

void CSIDevice_GCSteeringWheel::SendCommand(u32 command, u8 poll)
{
  const UCommand word(command);
  if (static_cast<EDirectCommands>(word.command) != EDirectCommands::CMD_FORCE)
  {
    CSIDevice_GCController::SendCommand(command, poll);
    return;
  }

  const int local_pad = NetPlay_InGamePadToLocalPad(m_device_number);
  if (local_pad < 4)
  {
    const u32 kind = word.parameter2 >> 1;
    // 9-bit force: 0 pulls fully left, 256 fully right, 128 is centred.
    const u32 force = ((word.parameter2 & 1u) << 8) | word.parameter1;
    if (kind == FORCE_MOTOR_ON)
      Pad::Rumble(local_pad, force / 128.0 - 1);
    else if (kind == FORCE_MOTOR_OFF)
      Pad::Rumble(local_pad, 0);
    else
      WARN_LOG_FMT(SERIALINTERFACE, "Wheel {}: unknown force command {}", m_device_number, kind);
  }

  if (poll == 0)
  {
    m_mode = static_cast<u8>(word.parameter2);
    INFO_LOG_FMT(SERIALINTERFACE, "Wheel {}: mode {}", m_device_number, m_mode);
  }
}
}  // namespace SerialInterface
