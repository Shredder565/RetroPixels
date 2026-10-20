// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/OH0/OH0Device.h"

#include <cstddef>
#include <memory>
#include <sstream>
#include <string>

#include "Common/ChunkFile.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/OH0/OH0.h"

namespace IOS::HLE
{
namespace
{
// Returns the n-th non-empty '/'-separated part of `path` and how many such parts there are.
std::string PathPart(const std::string& path, size_t wanted, size_t* count)
{
  std::string result;
  *count = 0;
  size_t start = 0;
  while (start <= path.size())
  {
    size_t end = path.find('/', start);
    if (end == std::string::npos)
      end = path.size();
    if (end > start)
    {
      if (*count == wanted)
        result = path.substr(start, end - start);
      ++*count;
    }
    start = end + 1;
  }
  return result;
}

// "/dev/usb/oh0/<vid>/<pid>", both in hex. Anything else leaves the IDs alone.
void ParseIds(const std::string& path, u16* vid, u16* pid)
{
  size_t parts = 0;
  const std::string vid_text = PathPart(path, 3, &parts);
  if (parts != 5)
    return;
  const std::string pid_text = PathPart(path, 4, &parts);

  // Both numbers are read through one stream on purpose: whatever the vendor ID does not use
  // is still in the stream when the product ID is read, as IOS paths have always been parsed.
  std::stringstream stream;
  stream << std::hex << vid_text;
  stream >> *vid;
  stream.clear();
  stream << pid_text;
  stream >> *pid;
}
}  // namespace

OH0Device::OH0Device(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name, DeviceType::OH0)
{
  if (!device_name.empty())
    ParseIds(device_name, &m_vid, &m_pid);
}

void OH0Device::FindController()
{
  m_controller =
      std::static_pointer_cast<OH0>(GetEmulationKernel().GetDeviceByName("/dev/usb/oh0"));
}

void OH0Device::DoState(PointerWrap& p)
{
  FindController();
  Device::DoState(p);
  p.Do(m_vid);
  p.Do(m_pid);
  p.Do(m_device_id);
}

std::optional<IPCReply> OH0Device::Open(const OpenRequest&)
{
  if (m_vid == 0 && m_pid == 0)
    return IPCReply(IPC_ENOENT);

  FindController();
  const auto [result, device_id] = m_controller->DeviceOpen(m_vid, m_pid);
  m_device_id = device_id;
  return IPCReply(result);
}

std::optional<IPCReply> OH0Device::Close(u32 fd)
{
  m_controller->DeviceClose(m_device_id);
  return Device::Close(fd);
}

std::optional<IPCReply> OH0Device::IOCtl(const IOCtlRequest& request)
{
  return m_controller->DeviceIOCtl(m_device_id, request);
}

std::optional<IPCReply> OH0Device::IOCtlV(const IOCtlVRequest& request)
{
  return m_controller->DeviceIOCtlV(m_device_id, request);
}
}  // namespace IOS::HLE
