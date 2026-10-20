// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// ES content file descriptors: titles read their (decrypted, installed) contents through ES.

#include "Core/IOS/ES/ES.h"

#include <vector>

#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/FS/FileSystemProxy.h"
#include "Core/IOS/Uids.h"
#include "Core/System.h"

namespace IOS::HLE
{
s32 ESCore::OpenContent(const ES::TMDReader& tmd, u16 content_index, u32 uid, Ticks ticks)
{
  ES::Content content;
  if (!tmd.GetContent(content_index, &content))
    return ES_EINVAL;

  size_t cfd = 0;
  while (cfd < m_content_table.size() && m_content_table[cfd].m_opened)
    ++cfd;
  if (cfd == m_content_table.size())
    return FS_EFDEXHAUSTED;

  const u64 title_id = tmd.GetTitleId();
  const std::string path = GetContentPath(title_id, content, ticks);
  FSCore::ScopedFd fd =
      m_ios.GetFSCore().Open(PID_KERNEL, PID_KERNEL, path, FS::Mode::Read, {}, ticks);
  if (fd.Get() < 0)
    return static_cast<s32>(fd.Get());

  OpenedContent& slot = m_content_table[cfd];
  slot.m_opened = true;
  slot.m_fd = static_cast<u64>(fd.Release());
  slot.m_content = content;
  slot.m_title_id = title_id;
  slot.m_uid = uid;
  INFO_LOG_FMT(IOS_ES,
               "OpenContent: title ID {:016x}, UID {:#x}, content {:08x} (index {}) -> CFD {}",
               title_id, uid, content.id, content_index, cfd);
  return static_cast<s32>(cfd);
}

namespace
{
// Shared checks for operations on an open content fd. Returns the slot, or sets `error`.
template <typename Table> auto* LookUpContent(Table& table, u32 cfd, u32 uid, s32* error)
{
  using Slot = typename Table::value_type;
  Slot* slot = nullptr;
  if (cfd >= table.size())
    *error = ES_EINVAL;
  else if (table[cfd].m_uid != uid)
    *error = ES_EACCES;
  else if (!table[cfd].m_opened)
    *error = IPC_EINVAL;
  else
    slot = &table[cfd];
  return slot;
}
}  // namespace

s32 ESCore::ReadContent(u32 cfd, u8* buffer, u32 size, u32 uid, Ticks ticks)
{
  s32 error = 0;
  OpenedContent* slot = LookUpContent(m_content_table, cfd, uid, &error);
  if (slot == nullptr)
    return error;
  return m_ios.GetFSCore().Read(slot->m_fd, buffer, size, {}, ticks);
}

s32 ESCore::CloseContent(u32 cfd, u32 uid, Ticks ticks)
{
  s32 error = 0;
  OpenedContent* slot = LookUpContent(m_content_table, cfd, uid, &error);
  if (slot == nullptr)
    return error;

  m_ios.GetFSCore().Close(slot->m_fd, ticks);
  *slot = {};
  INFO_LOG_FMT(IOS_ES, "CloseContent: CFD {}", cfd);
  return IPC_SUCCESS;
}

s32 ESCore::SeekContent(u32 cfd, u32 offset, SeekMode mode, u32 uid, Ticks ticks)
{
  s32 error = 0;
  OpenedContent* slot = LookUpContent(m_content_table, cfd, uid, &error);
  if (slot == nullptr)
    return error;
  return m_ios.GetFSCore().Seek(slot->m_fd, offset, static_cast<FS::SeekMode>(mode), ticks);
}

// -----------------------------------------------------------------------------------------------
// ioctlvs. All of them reply after the IPC overhead plus whatever the FS work costs.

IPCReply ESDevice::OpenContent(u32 uid, const IOCtlVRequest& request)
{
  return MakeIPCReply(IPC_OVERHEAD_TICKS, [&](Ticks ticks) -> s32 {
    // in: title ID, ticket view, content index
    if (!request.HasNumberOfValidVectors(3, 0) || request.in_vectors[0].size != sizeof(u64) ||
        request.in_vectors[1].size != sizeof(ES::TicketView) ||
        request.in_vectors[2].size != sizeof(u32))
    {
      return ES_EINVAL;
    }

    // The ticket view is not checked against the caller.
    Memory::MemoryManager& memory = GetSystem().GetMemory();
    const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
    const u32 index = memory.Read_U32(request.in_vectors[2].address);

    const ES::TMDReader tmd = m_core.FindInstalledTMD(title_id, ticks);
    if (!tmd.IsValid())
      return FS_ENOENT;
    return m_core.OpenContent(tmd, static_cast<u16>(index), uid, ticks);
  });
}

IPCReply ESDevice::OpenActiveTitleContent(u32 caller_uid, const IOCtlVRequest& request)
{
  return MakeIPCReply(IPC_OVERHEAD_TICKS, [&](Ticks ticks) -> s32 {
    if (!request.HasNumberOfValidVectors(1, 0) || request.in_vectors[0].size != sizeof(u32))
      return ES_EINVAL;

    const u32 index = GetSystem().GetMemory().Read_U32(request.in_vectors[0].address);
    if (!m_core.m_title_context.active)
      return ES_EINVAL;

    // Only the running title itself (or the kernel, UID 0) may do this.
    const ES::TMDReader& active_tmd = m_core.m_title_context.tmd;
    ES::UIDSys uid_sys{GetEmulationKernel().GetFSCore()};
    const u32 title_uid = uid_sys.GetOrInsertUIDForTitle(active_tmd.GetTitleId());
    ticks.Add(uid_sys.GetTicks());
    if (caller_uid != 0 && caller_uid != title_uid)
      return ES_EACCES;

    return m_core.OpenContent(active_tmd, static_cast<u16>(index), caller_uid, ticks);
  });
}

IPCReply ESDevice::ReadContent(u32 uid, const IOCtlVRequest& request)
{
  return MakeIPCReply(IPC_OVERHEAD_TICKS, [&](Ticks ticks) -> s32 {
    if (!request.HasNumberOfValidVectors(1, 1) || request.in_vectors[0].size != sizeof(u32))
      return ES_EINVAL;

    Memory::MemoryManager& memory = GetSystem().GetMemory();
    const u32 cfd = memory.Read_U32(request.in_vectors[0].address);
    const IOCtlVRequest::IOVector& out = request.io_vectors[0];
    INFO_LOG_FMT(IOS_ES, "ReadContent(uid={:#x}, cfd={}, size={}, addr={:08x})", uid, cfd, out.size,
                 out.address);
    u8* buffer = memory.GetPointerForRange(out.address, out.size);
    return m_core.ReadContent(cfd, buffer, out.size, uid, ticks);
  });
}

IPCReply ESDevice::CloseContent(u32 uid, const IOCtlVRequest& request)
{
  return MakeIPCReply(IPC_OVERHEAD_TICKS, [&](Ticks ticks) -> s32 {
    if (!request.HasNumberOfValidVectors(1, 0) || request.in_vectors[0].size != sizeof(u32))
      return ES_EINVAL;

    const u32 cfd = GetSystem().GetMemory().Read_U32(request.in_vectors[0].address);
    return m_core.CloseContent(cfd, uid, ticks);
  });
}

IPCReply ESDevice::SeekContent(u32 uid, const IOCtlVRequest& request)
{
  return MakeIPCReply(IPC_OVERHEAD_TICKS, [&](Ticks ticks) -> s32 {
    // in: cfd, offset, whence (the sizes are not checked)
    if (!request.HasNumberOfValidVectors(3, 0))
      return ES_EINVAL;

    Memory::MemoryManager& memory = GetSystem().GetMemory();
    const u32 cfd = memory.Read_U32(request.in_vectors[0].address);
    const u32 offset = memory.Read_U32(request.in_vectors[1].address);
    const u32 whence = memory.Read_U32(request.in_vectors[2].address);
    return m_core.SeekContent(cfd, offset, static_cast<SeekMode>(whence), uid, ticks);
  });
}
}  // namespace IOS::HLE
