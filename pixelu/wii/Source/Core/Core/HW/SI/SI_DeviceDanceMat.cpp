// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/SI/SI_DeviceDanceMat.h"

#include <array>
#include <utility>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace SerialInterface
{
namespace
{
// Pad button -> mat panel bit. The D-pad is the blue arrows, the face
// buttons the orange ones, Z is "+" and Start is "-". X and Y only exist on
// the Active Life mat.
constexpr std::array<std::pair<u16, u16>, 10> PANEL_MAP = {{
    {PAD_BUTTON_LEFT, 0x0001},
    {PAD_BUTTON_RIGHT, 0x0002},
    {PAD_BUTTON_DOWN, 0x0004},
    {PAD_BUTTON_UP, 0x0008},
    {PAD_TRIGGER_Z, 0x0010},
    {PAD_BUTTON_A, 0x0100},
    {PAD_BUTTON_B, 0x0200},
    {PAD_BUTTON_Y, 0x0400},
    {PAD_BUTTON_X, 0x0800},
    {PAD_BUTTON_START, 0x1000},
}};

// The mat has no sticks; its low word is this fixed pattern.
constexpr u32 MAT_LOW_WORD = 0x8080ffff;
constexpr u32 MAT_CENTRED_STICK = 0x8080;
}  // namespace

CSIDevice_DanceMat::CSIDevice_DanceMat(Core::System& system, SIDevices device, int device_number)
    : CSIDevice_GCController(system, device, device_number)
{
}

int CSIDevice_DanceMat::RunBuffer(u8* buffer, int request_length)
{
  if (static_cast<EBufferCommands>(buffer[0]) != EBufferCommands::CMD_STATUS)
    return CSIDevice_GCController::RunBuffer(buffer, request_length);

  ISIDevice::RunBuffer(buffer, request_length);
  return CreateStatusResponse(SI_DANCEMAT, buffer);
}

u32 CSIDevice_DanceMat::MapPadStatus(const GCPadStatus& pad_status)
{
  u32 panels = 0;
  for (const auto& [button, panel] : PANEL_MAP)
  {
    if ((pad_status.button & button) != 0)
      panels |= panel;
  }
  return (panels << 16) | MAT_CENTRED_STICK;
}

DataResponse CSIDevice_DanceMat::GetData(u32& hi, u32& low)
{
  CSIDevice_GCController::GetData(hi, low);
  low = MAT_LOW_WORD;
  return DataResponse::Success;
}
}  // namespace SerialInterface
