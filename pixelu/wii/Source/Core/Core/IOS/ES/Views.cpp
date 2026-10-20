// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// ES ticket and TMD views (the reduced forms that titles get to see), and tickets from views.

#include "Core/IOS/ES/ES.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Common/Logging/Log.h"
#include "Core/CommonTitles.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/VersionInfo.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
// Workaround: disc games booted from the game list (and any title during netplay or movie
// recording/playback) may look for IOS tickets that a fresh NAND does not have, e.g. before an
// IOS reload. Pretend those IOS versions are installed in these cases, so that users do not
// need to install disc updates first. Booting the disc through the system menu does not get this.
bool ShouldReturnFakeViewsForIOSes(u64 title_id, const TitleContext& context)
{
  if (Core::WantsDeterminism())
    return true;
  const bool is_ios =
      IsTitleType(title_id, ES::TitleType::System) && title_id != Titles::SYSTEM_MENU;
  const bool running_disc = context.active && ES::IsDiscTitle(context.tmd.GetTitleId());
  return is_ios && SConfig::GetInstance().m_disc_booted_from_game_list && running_disc;
}

// Largest TMD that the DI view calls accept.
constexpr u32 DI_TMD_SIZE_LIMIT = 4 * 1024 * 1024;
}  // namespace

// -----------------------------------------------------------------------------------------------
// Ticket views

IPCReply ESDevice::GetTicketViewCount(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const ES::TicketReader ticket = m_core.FindSignedTicket(title_id);

  u32 count = 0;
  if (!IsEmulated(title_id))
  {
    ERROR_LOG_FMT(IOS_ES, "GetViewCount: Dolphin doesn't emulate IOS title {:016x}", title_id);
  }
  else if (ShouldReturnFakeViewsForIOSes(title_id, m_core.m_title_context))
  {
    count = 1;
    WARN_LOG_FMT(IOS_ES, "GetViewCount: Faking IOS title {:016x} being present", title_id);
  }
  else if (ticket.IsValid())
  {
    count = static_cast<u32>(ticket.GetNumberOfTickets());
  }

  INFO_LOG_FMT(IOS_ES, "IOCTL_ES_GETVIEWCNT for titleID: {:016x} (View Count = {})", title_id,
               count);
  memory.Write_U32(count, request.io_vectors[0].address);
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetTicketViews(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 1))
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const u32 max_views = memory.Read_U32(request.in_vectors[1].address);
  const u32 out = request.io_vectors[0].address;
  const ES::TicketReader ticket = m_core.FindSignedTicket(title_id);

  if (!IsEmulated(title_id))
  {
    ERROR_LOG_FMT(IOS_ES, "GetViews: Dolphin doesn't emulate IOS title {:016x}", title_id);
  }
  else if (ticket.IsValid())
  {
    const u32 count = std::min(max_views, static_cast<u32>(ticket.GetNumberOfTickets()));
    for (u32 n = 0; n < count; ++n)
    {
      const std::vector<u8> view = ticket.GetRawTicketView(n);
      memory.CopyToEmu(out + n * static_cast<u32>(sizeof(ES::TicketView)), view.data(),
                       view.size());
    }
  }
  else if (ShouldReturnFakeViewsForIOSes(title_id, m_core.m_title_context))
  {
    memory.Memset(out, 0, sizeof(ES::TicketView));
    WARN_LOG_FMT(IOS_ES, "GetViews: Faking IOS title {:016x} being present", title_id);
  }

  INFO_LOG_FMT(IOS_ES, "IOCTL_ES_GETVIEWS for titleID: {:016x} (MaxViews = {})", title_id,
               max_views);
  return IPCReply(IPC_SUCCESS);
}

// -----------------------------------------------------------------------------------------------
// Tickets from views

// With `ticket` null, only the ticket size is reported. Otherwise the signed ticket the view
// refers to is copied out, provided the running title may export it.
ReturnCode ESCore::GetTicketFromView(const u8* ticket_view, u8* ticket, u32* ticket_size,
                                     std::optional<u8> desired_version) const
{
  const u64 title_id = Common::swap64(ticket_view + offsetof(ES::TicketView, title_id));
  const u64 ticket_id = Common::swap64(ticket_view + offsetof(ES::TicketView, ticket_id));
  const u8 version =
      desired_version ? *desired_version : ticket_view[offsetof(ES::TicketView, version)];

  const ES::TicketReader installed = FindSignedTicket(title_id, version);
  if (!installed.IsValid())
    return ES_NO_TICKET;

  if (ticket == nullptr)
  {
    *ticket_size = installed.GetTicketSize();
    return IPC_SUCCESS;
  }

  const std::vector<u8> bytes = installed.GetRawTicket(ticket_id);
  if (bytes.empty())
    return ES_NO_TICKET;
  if (!m_title_context.active)
    return ES_EINVAL;

  // IOS's export check: the running title must match the ticket's permitted title mask, and
  // the low nibble of title_export_allowed must be 5.
  const auto low_title_id = static_cast<u32>(m_title_context.tmd.GetTitleId());
  const u32 mask = Common::swap32(bytes.data() + offsetof(ES::Ticket, permitted_title_mask));
  const u32 permitted = Common::swap32(bytes.data() + offsetof(ES::Ticket, permitted_title_id));
  const u8 export_flags = bytes[offsetof(ES::Ticket, title_export_allowed)];
  const bool title_ok = low_title_id != 0 && (low_title_id & ~mask) == permitted;
  if (!title_ok || (export_flags & 0xf) != 5)
    return ES_EACCES;

  std::memcpy(ticket, bytes.data(), bytes.size());
  return IPC_SUCCESS;
}

IPCReply ESDevice::GetV0TicketFromView(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1) ||
      request.in_vectors[0].size != sizeof(ES::TicketView) ||
      request.io_vectors[0].size != sizeof(ES::Ticket))
  {
    return IPCReply(ES_EINVAL);
  }

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u8* view = memory.GetPointerForRange(request.in_vectors[0].address, sizeof(ES::TicketView));
  u8* out = memory.GetPointerForRange(request.io_vectors[0].address, sizeof(ES::Ticket));
  // Always the v0 ticket, whatever version the view names.
  return IPCReply(m_core.GetTicketFromView(view, out, nullptr, u8{0}));
}

IPCReply ESDevice::GetTicketSizeFromView(const IOCtlVRequest& request)
{
  u32 size = 0;
  if (!request.HasNumberOfValidVectors(1, 1) ||
      request.in_vectors[0].size != sizeof(ES::TicketView) ||
      request.io_vectors[0].size != sizeof(size))
  {
    return IPCReply(ES_EINVAL);
  }

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u8* view = memory.GetPointerForRange(request.in_vectors[0].address, sizeof(ES::TicketView));
  const ReturnCode ret = m_core.GetTicketFromView(view, nullptr, &size, std::nullopt);
  // Written whether or not the lookup worked.
  memory.Write_U32(size, request.io_vectors[0].address);
  return IPCReply(ret);
}

IPCReply ESDevice::GetTicketFromView(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 1) ||
      request.in_vectors[0].size != sizeof(ES::TicketView) ||
      request.in_vectors[1].size != sizeof(u32))
  {
    return IPCReply(ES_EINVAL);
  }

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  u32 size = memory.Read_U32(request.in_vectors[1].address);
  if (size != request.io_vectors[0].size)
    return IPCReply(ES_EINVAL);

  const u8* view = memory.GetPointerForRange(request.in_vectors[0].address, sizeof(ES::TicketView));
  u8* out = memory.GetPointerForRange(request.io_vectors[0].address, size);
  return IPCReply(m_core.GetTicketFromView(view, out, &size, std::nullopt));
}

// -----------------------------------------------------------------------------------------------
// TMD views of installed titles

IPCReply ESDevice::GetTMDViewSize(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const ES::TMDReader tmd = m_core.FindInstalledTMD(title_id);
  if (!tmd.IsValid())
    return IPCReply(FS_ENOENT);

  const auto size = static_cast<u32>(tmd.GetRawView().size());
  memory.Write_U32(size, request.io_vectors[0].address);
  INFO_LOG_FMT(IOS_ES, "GetTMDViewSize: {} bytes for title {:016x}", size, title_id);
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::GetTMDViews(const IOCtlVRequest& request)
{
  // in: title ID, view size; io: view
  if (!request.HasNumberOfValidVectors(2, 1) || request.in_vectors[0].size != sizeof(u64) ||
      request.in_vectors[1].size != sizeof(u32))
  {
    return IPCReply(ES_EINVAL);
  }
  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const IOCtlVRequest::IOVector& out = request.io_vectors[0];
  if (memory.Read_U32(request.in_vectors[1].address) != out.size)
    return IPCReply(ES_EINVAL);

  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const ES::TMDReader tmd = m_core.FindInstalledTMD(title_id);
  if (!tmd.IsValid())
    return IPCReply(FS_ENOENT);

  const std::vector<u8> view = tmd.GetRawView();
  if (view.size() > out.size)
    return IPCReply(ES_EINVAL);
  memory.CopyToEmu(out.address, view.data(), view.size());

  INFO_LOG_FMT(IOS_ES, "GetTMDView: {} bytes for title {:016x}", view.size(), title_id);
  return IPCReply(IPC_SUCCESS);
}

// -----------------------------------------------------------------------------------------------
// DI variants: these work on a TMD/ticket passed in, or on the running title when none is.
// An invalid TMD gives -1017 (ES_EINVAL); IOS only checks that all content records are there.

IPCReply ESDevice::DIGetTMDViewSize(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1) || request.in_vectors[0].size >= DI_TMD_SIZE_LIMIT ||
      request.io_vectors[0].size != sizeof(u32))
  {
    return IPCReply(ES_EINVAL);
  }

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const IOCtlVRequest::IOVector& tmd_in = request.in_vectors[0];
  size_t view_size;
  if (tmd_in.size == 0)
  {
    if (!m_core.m_title_context.active)
      return IPCReply(ES_EINVAL);
    view_size = m_core.m_title_context.tmd.GetRawView().size();
  }
  else
  {
    std::vector<u8> bytes(tmd_in.size);
    memory.CopyFromEmu(bytes.data(), tmd_in.address, bytes.size());
    const ES::TMDReader tmd{std::move(bytes)};
    if (!tmd.IsValid())
      return IPCReply(ES_EINVAL);
    view_size = tmd.GetRawView().size();
  }

  memory.Write_U32(static_cast<u32>(view_size), request.io_vectors[0].address);
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::DIGetTMDView(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 1) || request.in_vectors[0].size >= DI_TMD_SIZE_LIMIT)
    return IPCReply(ES_EINVAL);

  // in[1] must give the size of the output buffer.
  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const IOCtlVRequest::IOVector& out = request.io_vectors[0];
  if (request.in_vectors[1].size != sizeof(u32) ||
      memory.Read_U32(request.in_vectors[1].address) != out.size)
  {
    return IPCReply(ES_EINVAL);
  }

  const IOCtlVRequest::IOVector& tmd_in = request.in_vectors[0];
  std::vector<u8> view;
  if (tmd_in.size == 0)
  {
    if (!m_core.m_title_context.active)
      return IPCReply(ES_EINVAL);
    view = m_core.m_title_context.tmd.GetRawView();
  }
  else
  {
    std::vector<u8> bytes(tmd_in.size);
    memory.CopyFromEmu(bytes.data(), tmd_in.address, bytes.size());
    const ES::TMDReader tmd{std::move(bytes)};
    if (!tmd.IsValid())
      return IPCReply(ES_EINVAL);
    view = tmd.GetRawView();
  }

  if (view.size() > out.size)
    return IPCReply(ES_EINVAL);
  memory.CopyToEmu(out.address, view.data(), view.size());
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::DIGetTicketView(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1) ||
      request.io_vectors[0].size != sizeof(ES::TicketView))
  {
    return IPCReply(ES_EINVAL);
  }

  // in[0] is either a whole v0 ticket or empty (meaning: the running title's ticket).
  const IOCtlVRequest::IOVector& ticket_in = request.in_vectors[0];
  if (ticket_in.size != 0 && ticket_in.size != sizeof(ES::Ticket))
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  std::vector<u8> view;
  if (ticket_in.size == 0)
  {
    if (!m_core.m_title_context.active)
      return IPCReply(ES_EINVAL);
    view = m_core.m_title_context.ticket.GetRawTicketView(0);
  }
  else
  {
    std::vector<u8> bytes(ticket_in.size);
    memory.CopyFromEmu(bytes.data(), ticket_in.address, bytes.size());
    view = ES::TicketReader{std::move(bytes)}.GetRawTicketView(0);
  }

  memory.CopyToEmu(request.io_vectors[0].address, view.data(), view.size());
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::DIGetTMDSize(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1) || request.io_vectors[0].size != sizeof(u32) ||
      !m_core.m_title_context.active)
  {
    return IPCReply(ES_EINVAL);
  }

  const auto size = static_cast<u32>(m_core.m_title_context.tmd.GetBytes().size());
  GetSystem().GetMemory().Write_U32(size, request.io_vectors[0].address);
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::DIGetTMD(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1) || request.in_vectors[0].size != sizeof(u32))
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u32 size = memory.Read_U32(request.in_vectors[0].address);
  if (size != request.io_vectors[0].size || !m_core.m_title_context.active)
    return IPCReply(ES_EINVAL);

  const std::vector<u8>& bytes = m_core.m_title_context.tmd.GetBytes();
  if (static_cast<u32>(bytes.size()) > size)
    return IPCReply(ES_EINVAL);
  memory.CopyToEmu(request.io_vectors[0].address, bytes.data(), bytes.size());
  return IPCReply(IPC_SUCCESS);
}
}  // namespace IOS::HLE
