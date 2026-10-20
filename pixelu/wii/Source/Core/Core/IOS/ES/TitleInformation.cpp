// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// ES listings: installed and owned titles, stored contents, stored TMDs, shared contents.

#include "Core/IOS/ES/ES.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/System.h"

namespace IOS::HLE
{
// -----------------------------------------------------------------------------------------------
// Stored contents. The count variants write a u32 to io[0]; the list variants take the
// expected count in in[1] and write that many content IDs to io[0].

IPCReply ESDevice::GetStoredContentsCount(const ES::TMDReader& tmd, const IOCtlVRequest& request)
{
  const IOCtlVRequest::IOVector& out = request.io_vectors[0];
  if (out.size != sizeof(u32) || !tmd.IsValid())
    return IPCReply(ES_EINVAL);

  // The count is a 16-bit quantity in IOS.
  const auto count = static_cast<u16>(m_core.GetStoredContentsFromTMD(tmd).size());
  GetSystem().GetMemory().Write_U32(count, out.address);

  INFO_LOG_FMT(IOS_ES, "GetStoredContentsCount ({:#x}):  {} content(s) for {:016x}",
               request.request, count, tmd.GetTitleId());
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetStoredContents(const ES::TMDReader& tmd, const IOCtlVRequest& request)
{
  if (!tmd.IsValid())
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const IOCtlVRequest::IOVector& count_in = request.in_vectors[1];
  const IOCtlVRequest::IOVector& out = request.io_vectors[0];
  if (count_in.size != sizeof(u32))
    return IPCReply(ES_EINVAL);
  const u32 requested = memory.Read_U32(count_in.address);
  if (u64{out.size} != u64{requested} * sizeof(u32))
    return IPCReply(ES_EINVAL);

  const std::vector<ES::Content> stored = m_core.GetStoredContentsFromTMD(tmd);
  const u32 count = std::min(static_cast<u32>(stored.size()), requested);
  for (u32 n = 0; n < count; ++n)
    memory.Write_U32(stored[n].id, out.address + n * static_cast<u32>(sizeof(u32)));
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetStoredContentsCount(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1) || request.in_vectors[0].size != sizeof(u64))
    return IPCReply(ES_EINVAL);

  const u64 title_id = GetSystem().GetMemory().Read_U64(request.in_vectors[0].address);
  const ES::TMDReader tmd = m_core.FindInstalledTMD(title_id);
  return tmd.IsValid() ? GetStoredContentsCount(tmd, request) : IPCReply(FS_ENOENT);
}

IPCReply ESDevice::GetStoredContents(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 1) || request.in_vectors[0].size != sizeof(u64))
    return IPCReply(ES_EINVAL);

  const u64 title_id = GetSystem().GetMemory().Read_U64(request.in_vectors[0].address);
  const ES::TMDReader tmd = m_core.FindInstalledTMD(title_id);
  return tmd.IsValid() ? GetStoredContents(tmd, request) : IPCReply(FS_ENOENT);
}

namespace
{
std::vector<u8> CopyTMDFromVector(Memory::MemoryManager& memory, const IOCtlVRequest& request)
{
  const IOCtlVRequest::IOVector& in = request.in_vectors[0];
  std::vector<u8> bytes(in.size);
  memory.CopyFromEmu(bytes.data(), in.address, bytes.size());
  return bytes;
}
}  // namespace

// Same as above, but for a TMD passed in by the caller (in[0]) instead of an installed one.
IPCReply ESDevice::GetTMDStoredContentsCount(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return IPCReply(ES_EINVAL);

  const ES::TMDReader tmd{CopyTMDFromVector(GetSystem().GetMemory(), request)};
  return GetStoredContentsCount(tmd, request);
}

IPCReply ESDevice::GetTMDStoredContents(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 1))
    return IPCReply(ES_EINVAL);

  const ES::TMDReader tmd{CopyTMDFromVector(GetSystem().GetMemory(), request)};
  if (!tmd.IsValid())
    return IPCReply(ES_EINVAL);

  // Unlike the count variant, the listing requires a correctly signed TMD.
  std::vector<u8> cert_store;
  ReturnCode ret = m_core.ReadCertStore(&cert_store);
  if (ret == IPC_SUCCESS)
  {
    ret = m_core.VerifyContainer(ESCore::VerifyContainerType::TMD,
                                 ESCore::VerifyMode::UpdateCertStore, tmd, cert_store);
  }
  if (ret != IPC_SUCCESS)
    return IPCReply(ret);
  return GetStoredContents(tmd, request);
}

// -----------------------------------------------------------------------------------------------
// Title lists

IPCReply ESDevice::GetTitleCount(std::span<const u64> titles, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1) || request.io_vectors[0].size != sizeof(u32))
    return IPCReply(ES_EINVAL);

  GetSystem().GetMemory().Write_U32(static_cast<u32>(titles.size()), request.io_vectors[0].address);
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetTitles(std::span<const u64> titles, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const size_t limit = memory.Read_U32(request.in_vectors[0].address);
  const size_t count = std::min(limit, titles.size());
  const u32 out = request.io_vectors[0].address;
  for (size_t n = 0; n < count; ++n)
  {
    memory.Write_U64(titles[n], out + static_cast<u32>(n * sizeof(u64)));
    INFO_LOG_FMT(IOS_ES, "     title {:016x}", titles[n]);
  }
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetTitleCount(const IOCtlVRequest& request)
{
  const std::vector<u64> installed = m_core.GetInstalledTitles();
  INFO_LOG_FMT(IOS_ES, "GetTitleCount: {} titles", installed.size());
  return GetTitleCount(installed, request);
}

IPCReply ESDevice::GetTitles(const IOCtlVRequest& request)
{
  const std::vector<u64> installed = m_core.GetInstalledTitles();
  return GetTitles(installed, request);
}

IPCReply ESDevice::GetOwnedTitleCount(const IOCtlVRequest& request)
{
  const std::vector<u64> owned = m_core.GetTitlesWithTickets();
  INFO_LOG_FMT(IOS_ES, "GetOwnedTitleCount: {} titles", owned.size());
  return GetTitleCount(owned, request);
}

IPCReply ESDevice::GetOwnedTitles(const IOCtlVRequest& request)
{
  const std::vector<u64> owned = m_core.GetTitlesWithTickets();
  return GetTitles(owned, request);
}

// -----------------------------------------------------------------------------------------------
// Stored TMDs

IPCReply ESDevice::GetStoredTMDSize(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const ES::TMDReader tmd = m_core.FindInstalledTMD(title_id);
  if (!tmd.IsValid())
    return IPCReply(FS_ENOENT);

  const auto size = static_cast<u32>(tmd.GetBytes().size());
  memory.Write_U32(size, request.io_vectors[0].address);
  INFO_LOG_FMT(IOS_ES, "GetStoredTMDSize: {} bytes  for {:016x}", size, title_id);
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetStoredTMD(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 1))
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const ES::TMDReader tmd = m_core.FindInstalledTMD(title_id);
  if (!tmd.IsValid())
    return IPCReply(FS_ENOENT);

  // in[1] holds a size that is only logged; the output buffer must match the TMD exactly.
  const u32 caller_size = memory.Read_U32(request.in_vectors[1].address);
  const std::vector<u8>& bytes = tmd.GetBytes();
  if (bytes.size() != request.io_vectors[0].size)
    return IPCReply(ES_EINVAL);
  memory.CopyToEmu(request.io_vectors[0].address, bytes.data(), bytes.size());

  INFO_LOG_FMT(IOS_ES, "GetStoredTMD: title {:016x} (buffer size: {})", title_id, caller_size);
  return IPCReply(IPC_SUCCESS);
}

// -----------------------------------------------------------------------------------------------
// Misc

IPCReply ESDevice::GetBoot2Version(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1))
    return IPCReply(ES_EINVAL);

  INFO_LOG_FMT(IOS_ES, "IOCTL_ES_GETBOOT2VERSION");
  // boot2v4, the most recent version.
  GetSystem().GetMemory().Write_U32(4, request.io_vectors[0].address);
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetSharedContentsCount(const IOCtlVRequest& request) const
{
  if (!request.HasNumberOfValidVectors(0, 1) || request.io_vectors[0].size != sizeof(u32))
    return IPCReply(ES_EINVAL);

  const u32 count = m_core.GetSharedContentsCount();
  GetSystem().GetMemory().Write_U32(count, request.io_vectors[0].address);
  INFO_LOG_FMT(IOS_ES, "GetSharedContentsCount: {} contents", count);
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetSharedContents(const IOCtlVRequest& request) const
{
  if (!request.HasNumberOfValidVectors(1, 1) || request.in_vectors[0].size != sizeof(u32))
    return IPCReply(ES_EINVAL);

  constexpr u32 HASH_SIZE = 20;
  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u32 requested = memory.Read_U32(request.in_vectors[0].address);
  // 32-bit arithmetic, as in IOS.
  if (request.io_vectors[0].size != HASH_SIZE * requested)
    return IPCReply(ES_EINVAL);

  const std::vector<std::array<u8, 20>> hashes = m_core.GetSharedContents();
  const u32 count = std::min(static_cast<u32>(hashes.size()), requested);
  memory.CopyToEmu(request.io_vectors[0].address, hashes.data(), HASH_SIZE * count);

  INFO_LOG_FMT(IOS_ES, "GetSharedContents: {} contents ({} requested)", count, requested);
  return IPCReply(IPC_SUCCESS);
}
}  // namespace IOS::HLE
