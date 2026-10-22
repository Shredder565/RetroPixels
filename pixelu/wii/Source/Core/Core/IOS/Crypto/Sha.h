// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/Crypto/SHA1.h"
#include "Core/IOS/Device.h"

namespace IOS::HLE
{
// /dev/sha: the Starlet SHA-1 engine. The caller keeps the running hash state in its own
// memory and passes it in with every request.
class ShaDevice final : public EmulationDevice
{
public:
  ShaDevice(EmulationKernel& ios, const std::string& device_name);
  std::optional<IPCReply> Open(const OpenRequest& request) override;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;

  enum class ShaIoctlv : u32
  {
    InitState = 0x00,
    ContributeState = 0x01,
    FinalizeState = 0x02,
    ShaCommandUnknown = 0x0F
  };

  // The caller-held state: the five chaining words and the 64-bit message length counter
  // (stored as two 32-bit words, low word first).
  struct ShaContext
  {
    std::array<u32, 5> states;
    std::array<u32, 2> length;
  };

private:
  HLE::ReturnCode ProcessShaCommand(ShaIoctlv command, const IOCtlVRequest& request);
};
}  // namespace IOS::HLE
