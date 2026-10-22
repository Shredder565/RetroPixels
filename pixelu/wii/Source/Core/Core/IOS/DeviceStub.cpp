// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/DeviceStub.h"

#include <string_view>

#include "Common/Logging/Log.h"

namespace IOS::HLE
{
namespace
{
IPCReply PretendSuccess(const std::string& device, std::string_view what)
{
  WARN_LOG_FMT(IOS, "{}: stub device, reporting success for {}", device, what);
  return IPCReply(IPC_SUCCESS);
}
}  // namespace

std::optional<IPCReply> DeviceStub::Open(const OpenRequest&)
{
  m_is_active = true;
  return PretendSuccess(m_name, "Open");
}

std::optional<IPCReply> DeviceStub::IOCtl(const IOCtlRequest&)
{
  return PretendSuccess(m_name, "IOCtl");
}

std::optional<IPCReply> DeviceStub::IOCtlV(const IOCtlVRequest&)
{
  return PretendSuccess(m_name, "IOCtlV");
}
}  // namespace IOS::HLE
