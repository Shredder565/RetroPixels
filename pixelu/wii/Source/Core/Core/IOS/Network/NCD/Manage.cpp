// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Network/NCD/Manage.h"

#include <string>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Network.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/Network/MACUtils.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
// Result written when the lock is not held by the given handle.
constexpr u32 NCD_RESULT_NOT_LOCKED = static_cast<u32>(-3);
}  // namespace

NetNCDManageDevice::NetNCDManageDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
  m_config.ReadConfig(ios.GetFS().get());
}

void NetNCDManageDevice::DoState(PointerWrap& p)
{
  Device::DoState(p);
  p.Do(m_ipc_fd);
}

std::optional<IPCReply> NetNCDManageDevice::IOCtlV(const IOCtlVRequest& request)
{
  Memory::MemoryManager& memory = GetSystem().GetMemory();

  // Every command ends by storing an NCD result word in an io vector: normally the first one,
  // or for GETCONFIG the second one (then also repeated 4 bytes further on).
  u32 result = 0;
  bool result_in_second_vector = false;

  switch (request.request)
  {
  case IOCTLV_NCD_LOCKWIRELESSDRIVER:
  {
    if (!request.HasNumberOfValidVectors(0, 1) || request.io_vectors[0].size < 8)
      return IPCReply(IPC_EINVAL);

    if (m_ipc_fd == 0)
    {
      // Real NCD hands out the address of its internal fd structure; the fd itself is just as
      // good as a handle and is never 0.
      m_ipc_fd = request.fd;
      memory.Write_U32(request.fd, request.io_vectors[0].address + 4);
    }
    else
    {
      result = static_cast<u32>(IPC_EINVAL);
    }
    break;
  }

  case IOCTLV_NCD_UNLOCKWIRELESSDRIVER:
  {
    if (!request.HasNumberOfValidVectors(1, 1) || request.in_vectors[0].size < 4 ||
        request.io_vectors[0].size < 4)
    {
      return IPCReply(IPC_EINVAL);
    }

    if (memory.Read_U32(request.in_vectors[0].address) == m_ipc_fd)
      m_ipc_fd = 0;
    else
      result = NCD_RESULT_NOT_LOCKED;
    break;
  }

  case IOCTLV_NCD_GETCONFIG:
    INFO_LOG_FMT(IOS_NET, "{}: get config", GetDeviceName());
    m_config.WriteToMem(memory, request.io_vectors.at(0).address);
    result_in_second_vector = true;
    break;

  case IOCTLV_NCD_SETCONFIG:
    INFO_LOG_FMT(IOS_NET, "{}: set config", GetDeviceName());
    m_config.ReadFromMem(memory, request.in_vectors.at(0).address);
    break;

  case IOCTLV_NCD_READCONFIG:
    INFO_LOG_FMT(IOS_NET, "{}: read config", GetDeviceName());
    m_config.ReadConfig(m_ios.GetFS().get());
    m_config.WriteToMem(memory, request.io_vectors.at(0).address);
    break;

  case IOCTLV_NCD_WRITECONFIG:
    INFO_LOG_FMT(IOS_NET, "{}: write config", GetDeviceName());
    m_config.ReadFromMem(memory, request.in_vectors.at(0).address);
    m_config.WriteConfig(m_ios.GetFS().get());
    break;

  case IOCTLV_NCD_GETLINKSTATUS:
    // The emulated console is always on a working wired link.
    memory.Write_U32(Net::ConnectionSettings::LINK_WIRED, request.io_vectors.at(0).address + 4);
    break;

  case IOCTLV_NCD_GETWIRELESSMACADDRESS:
  {
    const Common::MACAddress mac = IOS::Net::GetMACAddress();
    memory.CopyToEmu(request.io_vectors.at(1).address, mac.data(), mac.size());
    break;
  }

  default:
    INFO_LOG_FMT(IOS_NET, "{}: unhandled ioctlv {:#x}", GetDeviceName(), request.request);
    break;
  }

  if (result_in_second_vector)
  {
    const u32 address = request.io_vectors.at(1).address;
    memory.Write_U32(result, address);
    memory.Write_U32(result, address + 4);
  }
  else
  {
    memory.Write_U32(result, request.io_vectors.at(0).address);
  }
  return IPCReply(IPC_SUCCESS);
}
}  // namespace IOS::HLE
