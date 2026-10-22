// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/Bluetooth/BTStub.h"

#include "Common/ChunkFile.h"
#include "Common/MsgHandler.h"
#include "Core/Core.h"

namespace IOS::HLE
{
std::optional<IPCReply> BluetoothStubDevice::Open(const OpenRequest&)
{
  PanicAlertFmtT("Bluetooth passthrough mode is enabled, but Dolphin was built without libusb."
                 " Passthrough mode cannot be used.");
  return IPCReply(IPC_ENOENT);
}

void BluetoothStubDevice::DoState(PointerWrap& p)
{
  // There is no state to restore, so a state that has this device cannot be loaded.
  Core::DisplayMessage("The current IPC_HLE_Device_usb is a stub. Aborting load.", 4000);
  p.SetVerifyMode();
}
}  // namespace IOS::HLE
