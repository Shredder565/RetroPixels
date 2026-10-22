// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/Crypto/AES.h"
#include "Core/IOS/Device.h"

namespace IOS::HLE
{
// /dev/aes: the Starlet AES-128-CBC engine as offered to the PPC.
class AesDevice final : public EmulationDevice
{
public:
  AesDevice(EmulationKernel& ios, const std::string& device_name);
  std::optional<IPCReply> Open(const OpenRequest& request) override;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;

  enum class AesIoctlv : u32
  {
    Copy = 0x00,
    Encrypt = 0x02,
    Decrypt = 0x03,
  };
};
}  // namespace IOS::HLE
