// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/SI/SI_DeviceGCAdapter.h"

#include "Common/CommonTypes.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/GCPad.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#include "InputCommon/GCAdapter.h"

namespace SerialInterface
{
CSIDevice_GCAdapter::CSIDevice_GCAdapter(Core::System& system, SIDevices device, int device_number)
    : CSIDevice_GCController(system, device, device_number)
{
  // Forget the port's last controller type so the adapter code flags the
  // next poll with PAD_GET_ORIGIN, as for a newly plugged pad.
  GCAdapter::ResetDeviceType(m_device_number);

  const int local_pad = NetPlay_InGamePadToLocalPad(m_device_number);
  if (local_pad < 4)
    m_simulate_konga = Config::Get(Config::GetInfoForSimulateKonga(local_pad));
}

GCPadStatus CSIDevice_GCAdapter::GetPadStatus()
{
  GCPadStatus pad = {};
  if (!NetPlay::IsNetPlayRunning())
    pad = GCAdapter::Input(m_device_number);

  HandleMoviePadStatus(m_system.GetMovie(), m_device_number, &pad);

  if ((pad.button & PAD_GET_ORIGIN) != 0)
    SetOrigin(pad);

  return pad;
}

int CSIDevice_GCAdapter::RunBuffer(u8* buffer, int request_length)
{
  // An empty adapter port answers "no device" (the SI_NONE word). That is
  // skipped when determinism is required, because the answer depends on the
  // local hardware and would desync NetPlay and movies.
  if (!Core::WantsDeterminism() && !GCAdapter::DeviceConnected(m_device_number))
  {
    buffer[0] = static_cast<u8>(SI_NONE >> 24);
    buffer[1] = static_cast<u8>(SI_NONE >> 16);
    buffer[2] = static_cast<u8>(SI_NONE >> 8);
    buffer[3] = static_cast<u8>(SI_NONE);
    return 4;
  }
  return CSIDevice_GCController::RunBuffer(buffer, request_length);
}

DataResponse CSIDevice_GCAdapter::GetData(u32& hi, u32& low)
{
  CSIDevice_GCController::GetData(hi, low);
  if (m_simulate_konga)
    hi &= CSIDevice_TaruKonga::HI_BUTTON_MASK;
  return DataResponse::Success;
}

void CSIDevice_GCController::Rumble(int pad_num, ControlState strength, SIDevices device)
{
  if (device == SIDEVICE_WIIU_ADAPTER)
    GCAdapter::Output(pad_num, static_cast<u8>(strength));
  else if (SIDevice_IsGCController(device))
    Pad::Rumble(pad_num, strength);
}
}  // namespace SerialInterface
