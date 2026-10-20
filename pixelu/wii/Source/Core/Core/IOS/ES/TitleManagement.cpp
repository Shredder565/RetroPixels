// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// ES title installation, export (backup to SD) and removal.

#include "Core/IOS/ES/ES.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Core/CommonTitles.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/Uids.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
constexpr FS::Modes ES_FILE_MODES{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::None};

// The IV used for a content's data: its index (big endian) in the first two bytes, then zeroes.
void SetContentIV(std::array<u8, 16>& iv, u16 content_index)
{
  iv[0] = static_cast<u8>(content_index >> 8);
  iv[1] = static_cast<u8>(content_index);
}

ReturnCode WriteTicket(FS::FileSystem* fs, const ES::TicketReader& ticket)
{
  const u64 title_id = ticket.GetTitleId();
  const std::string path = ticket.IsV1Ticket() ? Common::GetV1TicketFileName(title_id) :
                                                 Common::GetTicketFileName(title_id);
  fs->CreateFullPath(PID_KERNEL, PID_KERNEL, path, 0, ES_FILE_MODES);
  const auto file = fs->CreateAndOpenFile(PID_KERNEL, PID_KERNEL, path, ES_FILE_MODES);
  if (!file)
    return FS::ConvertResult(file.error());
  const std::vector<u8>& bytes = ticket.GetBytes();
  if (!file->Write(bytes.data(), bytes.size()))
    return ES_EIO;
  return IPC_SUCCESS;
}

// The key for title backups (export, and ImportTmd when restoring one). This is normally the
// PRNG key. Some IOS versions wrongly used an all-zero key instead, and later versions kept
// doing so for the titles that were affected, so that existing backups stay usable.
ReturnCode InitBackupKey(u64 caller_title_id, u32 caller_flags, IOSC& iosc, IOSC::Handle* key)
{
  const u64 any_region = caller_title_id | 0xff;
  constexpr u32 AFFECTED_FLAGS = ES::TITLE_TYPE_0x10 | ES::TITLE_TYPE_DATA;
  const bool affected_title =
      any_region == 0x00010005'735841ff || any_region - 0x00010005'735a41ff <= 0x700;
  const bool uses_zero_key = any_region != Titles::SYSTEM_MENU &&
                             (caller_flags & AFFECTED_FLAGS) == AFFECTED_FLAGS && affected_title;
  if (!uses_zero_key)
  {
    *key = IOSC::HANDLE_PRNG_KEY;
    return IPC_SUCCESS;
  }

  const ReturnCode ret =
      iosc.CreateObject(key, IOSC::TYPE_SECRET_KEY, IOSC::ObjectSubType::AES128, PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;
  constexpr std::array<u8, 16> zero_key{};
  return iosc.ImportSecretKey(*key, zero_key.data(), PID_ES);
}

// Frees the context's key object (if any) and clears the whole import/export state.
void ResetTitleImportContext(ESCore::Context* context, IOSC& iosc)
{
  const IOSC::Handle key = context->title_import_export.key_handle;
  if (key != 0)
    iosc.DeleteObject(key, PID_ES);
  context->title_import_export = {};
}

// The title key, decrypted with the common key the ticket names, as a new IOSC object.
ReturnCode InitTitleImportKey(const std::vector<u8>& ticket, IOSC& iosc, IOSC::Handle* handle)
{
  const ReturnCode ret =
      iosc.CreateObject(handle, IOSC::TYPE_SECRET_KEY, IOSC::ObjectSubType::AES128, PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;

  const u8 common_key = ticket[offsetof(ES::Ticket, common_key_index)];
  if (common_key >= IOSC::COMMON_KEY_HANDLES.size())
    return ES_INVALID_TICKET;

  std::array<u8, 16> iv{};
  std::memcpy(iv.data(), ticket.data() + offsetof(ES::Ticket, title_id), sizeof(u64));
  return iosc.ImportSecretKey(*handle, IOSC::COMMON_KEY_HANDLES[common_key], iv.data(),
                              ticket.data() + offsetof(ES::Ticket, title_key), PID_ES);
}

std::string GetImportContentPath(u64 title_id, u32 content_id)
{
  return fmt::format("{}/content/{:08x}.app", Common::GetImportTitlePath(title_id), content_id);
}

// System titles must have every non-optional content in place before an import can finish.
// The title still lives under /import at this point.
bool HasAllRequiredContents(Kernel& ios, const ES::TMDReader& tmd)
{
  const u64 title_id = tmd.GetTitleId();
  const ES::SharedContentMap shared{ios.GetFSCore()};
  for (const ES::Content& content : tmd.GetContents())
  {
    if (content.IsOptional())
      continue;
    const bool present =
        content.IsShared() ?
            shared.GetFilenameFromSHA1(content.sha1).has_value() :
            ios.GetFS()
                ->GetMetadata(PID_KERNEL, PID_KERNEL, GetImportContentPath(title_id, content.id))
                .has_value();
    if (!present)
      return false;
  }
  return true;
}

// IOS refuses to delete system titles up to and including 1-101 (boot2, IOSes, BC, MIOS).
bool CanDeleteTitle(u64 title_id)
{
  const auto high = static_cast<u32>(title_id >> 32);
  const auto low = static_cast<u32>(title_id);
  return high != 0x00000001 || low > 0x101;
}
}  // namespace

void ESCore::TitleImportExportContext::DoState(PointerWrap& p)
{
  p.Do(valid);
  p.Do(key_handle);
  tmd.DoState(p);
  TitleImportExportContext::ContentContext& c = content;
  p.Do(c.valid);
  p.Do(c.id);
  p.Do(c.iv);
  p.Do(c.buffer);
}

// -----------------------------------------------------------------------------------------------
// Tickets

ReturnCode ESCore::ImportTicket(const std::vector<u8>& ticket_bytes,
                                const std::vector<u8>& cert_chain, TicketImportType type,
                                VerifySignature verify_signature)
{
  ES::TicketReader ticket{ticket_bytes};
  if (!ticket.IsValid())
    return ES_EINVAL;

  IOSC& iosc = m_ios.GetIOSC();
  const u32 ticket_device = ticket.GetDeviceId();
  const u32 console_device = iosc.GetDeviceId();
  // A non-zero device ID marks a ticket bound to one console.
  if (type == TicketImportType::PossiblyPersonalised && ticket_device != 0)
  {
    if (ticket_device != console_device)
    {
      WARN_LOG_FMT(IOS_ES, "Device ID mismatch: ticket {:08x}, device {:08x}", ticket_device,
                   console_device);
      return ES_DEVICE_ID_MISMATCH;
    }
    const ReturnCode ret = ticket.Unpersonalise(iosc);
    if (ret < 0)
    {
      ERROR_LOG_FMT(IOS_ES, "ImportTicket: Failed to unpersonalise ticket for {:016x} ({})",
                    ticket.GetTitleId(), std::to_underlying(ret));
      return ret;
    }
  }

  if (verify_signature == VerifySignature::Yes)
  {
    const ReturnCode ret = VerifyContainer(VerifyContainerType::Ticket, VerifyMode::UpdateCertStore,
                                           ticket, cert_chain);
    if (ret != IPC_SUCCESS)
      return ret;
  }

  if (const ReturnCode ret = WriteTicket(m_ios.GetFS().get(), ticket); ret != IPC_SUCCESS)
    return ret;

  INFO_LOG_FMT(IOS_ES, "ImportTicket: Imported ticket for title {:016x}", ticket.GetTitleId());
  return IPC_SUCCESS;
}

IPCReply ESDevice::ImportTicket(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(3, 0))
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const auto copy_in = [&memory](const IOCtlVRequest::IOVector& vec) {
    std::vector<u8> bytes(vec.size);
    memory.CopyFromEmu(bytes.data(), vec.address, vec.size);
    return bytes;
  };
  const std::vector<u8> ticket = copy_in(request.in_vectors[0]);
  const std::vector<u8> certs = copy_in(request.in_vectors[1]);
  return IPCReply(m_core.ImportTicket(ticket, certs));
}

// -----------------------------------------------------------------------------------------------
// Title import

ReturnCode ESCore::ImportTmd(Context& context, const std::vector<u8>& tmd_bytes,
                             u64 caller_title_id, u32 caller_title_flags)
{
  INFO_LOG_FMT(IOS_ES, "ImportTmd");

  // Ioctlv 0x2b (restoring a backup) only sets the TMD for the import that follows; IOS puts it
  // in /tmp/title.tmd and nowhere else.
  IOSC& iosc = m_ios.GetIOSC();
  ResetTitleImportContext(&context, iosc);
  auto& import = context.title_import_export;
  import.tmd.SetBytes(tmd_bytes);
  if (!import.tmd.IsValid())
    return ES_EINVAL;

  std::vector<u8> cert_store;
  ReturnCode ret = ReadCertStore(&cert_store);
  if (ret != IPC_SUCCESS)
    return ret;

  ret = VerifyContainer(VerifyContainerType::TMD, VerifyMode::UpdateCertStore, import.tmd,
                        cert_store);
  if (ret != IPC_SUCCESS)
  {
    ERROR_LOG_FMT(IOS_ES, "ImportTmd: VerifyContainer failed with error {}",
                  std::to_underlying(ret));
    return ret;
  }

  if (!InitImport(import.tmd))
  {
    ERROR_LOG_FMT(IOS_ES, "ImportTmd: Failed to initialise title import");
    return ES_EIO;
  }

  ret = InitBackupKey(caller_title_id, caller_title_flags, iosc, &import.key_handle);
  if (ret != IPC_SUCCESS)
  {
    ERROR_LOG_FMT(IOS_ES, "ImportTmd: InitBackupKey failed with error {}", std::to_underlying(ret));
    return ret;
  }

  INFO_LOG_FMT(IOS_ES, "ImportTmd: All checks passed, marking context as valid");
  import.valid = true;
  return IPC_SUCCESS;
}

IPCReply ESDevice::ImportTmd(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0) || !ES::IsValidTMDSize(request.in_vectors[0].size))
    return IPCReply(ES_EINVAL);

  const IOCtlVRequest::IOVector& in = request.in_vectors[0];
  std::vector<u8> tmd(in.size);
  GetSystem().GetMemory().CopyFromEmu(tmd.data(), in.address, in.size);

  const ES::TMDReader& active_tmd = m_core.m_title_context.tmd;
  return IPCReply(
      m_core.ImportTmd(context, tmd, active_tmd.GetTitleId(), active_tmd.GetTitleFlags()));
}

ReturnCode ESCore::ImportTitleInit(Context& context, const std::vector<u8>& tmd_bytes,
                                   const std::vector<u8>& cert_chain,
                                   VerifySignature verify_signature)
{
  INFO_LOG_FMT(IOS_ES, "ImportTitleInit");
  IOSC& iosc = m_ios.GetIOSC();
  ResetTitleImportContext(&context, iosc);
  auto& import = context.title_import_export;
  import.tmd.SetBytes(tmd_bytes);
  if (!import.tmd.IsValid())
  {
    ERROR_LOG_FMT(IOS_ES, "Invalid TMD while adding title (size = {})", tmd_bytes.size());
    return ES_EINVAL;
  }

  const u64 title_id = import.tmd.GetTitleId();
  // Complete whatever an earlier, interrupted import of this title left behind.
  FinishStaleImport(title_id);

  const bool verify = verify_signature == VerifySignature::Yes;
  ReturnCode ret = IPC_SUCCESS;
  if (verify)
  {
    ret = VerifyContainer(VerifyContainerType::TMD, VerifyMode::UpdateCertStore, import.tmd,
                          cert_chain);
    if (ret != IPC_SUCCESS)
      return ret;
  }

  const ES::TicketReader ticket = FindSignedTicket(title_id);
  if (!ticket.IsValid())
    return ES_NO_TICKET;

  if (verify)
  {
    // The ticket was installed earlier, so its issuers are already in the store.
    std::vector<u8> cert_store;
    ret = ReadCertStore(&cert_store);
    if (ret != IPC_SUCCESS)
      return ret;
    ret = VerifyContainer(VerifyContainerType::Ticket, VerifyMode::DoNotUpdateCertStore, ticket,
                          cert_store);
    if (ret != IPC_SUCCESS)
      return ret;
  }

  ret = InitTitleImportKey(ticket.GetBytes(), iosc, &import.key_handle);
  if (ret != IPC_SUCCESS)
    return ret;

  if (!InitImport(import.tmd))
    return ES_EIO;

  import.valid = true;
  return IPC_SUCCESS;
}

IPCReply ESDevice::ImportTitleInit(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(4, 0) || !ES::IsValidTMDSize(request.in_vectors[0].size))
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const IOCtlVRequest::IOVector& tmd_in = request.in_vectors[0];
  const IOCtlVRequest::IOVector& certs_in = request.in_vectors[1];
  std::vector<u8> tmd(tmd_in.size);
  memory.CopyFromEmu(tmd.data(), tmd_in.address, tmd_in.size);
  std::vector<u8> certs(certs_in.size);
  memory.CopyFromEmu(certs.data(), certs_in.address, certs_in.size);
  return IPCReply(m_core.ImportTitleInit(context, tmd, certs));
}

ReturnCode ESCore::ImportContentBegin(Context& context, u64 title_id, u32 content_id)
{
  auto& import = context.title_import_export;
  if (import.content.valid)
  {
    ERROR_LOG_FMT(IOS_ES, "Trying to add content when we haven't finished adding "
                          "another content. Unsupported.");
    return ES_EINVAL;
  }
  import.content = {};
  import.content.id = content_id;

  INFO_LOG_FMT(IOS_ES, "ImportContentBegin: title {:016x}, content ID {:08x}", title_id,
               content_id);

  if (!import.valid)
    return ES_EINVAL;

  if (title_id != import.tmd.GetTitleId())
  {
    ERROR_LOG_FMT(IOS_ES, "ImportContentBegin: title id {:016x} != TMD title id {:016x}, ignoring",
                  title_id, import.tmd.GetTitleId());
    return ES_EINVAL;
  }

  ES::Content info;
  if (!import.tmd.FindContentById(content_id, &info))
    return ES_EINVAL;
  SetContentIV(import.content.iv, info.index);
  import.content.valid = true;

  // IOS hands out a content fd for the following calls. Only one content can be imported at a
  // time here (see the check above), so the fd is always 0.
  return IPC_SUCCESS;
}

IPCReply ESDevice::ImportContentBegin(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 0))
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const u32 content_id = memory.Read_U32(request.in_vectors[1].address);
  return IPCReply(m_core.ImportContentBegin(context, title_id, content_id));
}

ReturnCode ESCore::ImportContentData(Context& context, u32 content_fd, const u8* data,
                                     u32 data_size)
{
  INFO_LOG_FMT(IOS_ES, "ImportContentData: content fd {:08x}, size {}", content_fd, data_size);
  // The data is buffered and only decrypted once the whole content has arrived.
  std::vector<u8>& buffer = context.title_import_export.content.buffer;
  buffer.insert(buffer.end(), data, data + data_size);
  return IPC_SUCCESS;
}

IPCReply ESDevice::ImportContentData(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 0))
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u32 content_fd = memory.Read_U32(request.in_vectors[0].address);
  const IOCtlVRequest::IOVector& data_in = request.in_vectors[1];
  const u8* data = memory.GetPointerForRange(data_in.address, data_in.size);
  return IPCReply(m_core.ImportContentData(context, content_fd, data, data_in.size));
}

ReturnCode ESCore::ImportContentEnd(Context& context, u32 content_fd)
{
  INFO_LOG_FMT(IOS_ES, "ImportContentEnd: content fd {:08x}", content_fd);

  auto& import = context.title_import_export;
  if (!import.valid || !import.content.valid)
    return ES_EINVAL;

  const std::vector<u8>& encrypted = import.content.buffer;
  std::vector<u8> plain(encrypted.size());
  const ReturnCode decrypted =
      m_ios.GetIOSC().Decrypt(import.key_handle, import.content.iv.data(), encrypted.data(),
                              encrypted.size(), plain.data(), PID_ES);
  if (decrypted != IPC_SUCCESS)
    return decrypted;

  // The hash covers the content's real size; the buffer may carry padding beyond it.
  ES::Content info;
  import.tmd.FindContentById(import.content.id, &info);
  if (Common::SHA1::CalculateDigest(plain.data(), info.size) != info.sha1)
  {
    ERROR_LOG_FMT(IOS_ES, "ImportContentEnd: Hash for content {:08x} doesn't match", info.id);
    return ES_HASH_MISMATCH;
  }

  std::string destination;
  if (info.IsShared())
  {
    ES::SharedContentMap shared{m_ios.GetFSCore()};
    destination = shared.AddSharedContent(info.sha1);
  }
  else
  {
    destination = GetImportContentPath(import.tmd.GetTitleId(), import.content.id);
  }

  // Write the content under /tmp with its final file name, then move it into place.
  const auto fs = m_ios.GetFS();
  const std::string staging = "/tmp/" + destination.substr(destination.rfind('/') + 1);
  {
    const auto file = fs->CreateAndOpenFile(PID_KERNEL, PID_KERNEL, staging, ES_FILE_MODES);
    if (!file || !file->Write(plain.data(), info.size))
    {
      ERROR_LOG_FMT(IOS_ES, "ImportContentEnd: Failed to write to {}", staging);
      return ES_EIO;
    }
  }

  const FS::ResultCode moved = fs->Rename(PID_KERNEL, PID_KERNEL, staging, destination);
  if (moved != FS::ResultCode::Success)
  {
    fs->Delete(PID_KERNEL, PID_KERNEL, staging);
    ERROR_LOG_FMT(IOS_ES, "ImportContentEnd: Failed to move content to {}", destination);
    return FS::ConvertResult(moved);
  }

  import.content = {};
  return IPC_SUCCESS;
}

IPCReply ESDevice::ImportContentEnd(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0))
    return IPCReply(ES_EINVAL);

  const u32 content_fd = GetSystem().GetMemory().Read_U32(request.in_vectors[0].address);
  return IPCReply(m_core.ImportContentEnd(context, content_fd));
}

ReturnCode ESCore::ImportTitleDone(Context& context)
{
  auto& import = context.title_import_export;
  if (!import.valid || import.content.valid)
  {
    ERROR_LOG_FMT(IOS_ES,
                  "ImportTitleDone: No title import, or a content import is still in progress");
    return ES_EINVAL;
  }

  // 1-1 up to 1-101: boot2, IOS, BC and MIOS.
  const u64 title_id = import.tmd.GetTitleId();
  const bool is_system_title = title_id - u64{0x00000001'00000001} <= 0x100;
  if (is_system_title && !HasAllRequiredContents(m_ios, import.tmd))
  {
    ERROR_LOG_FMT(IOS_ES, "ImportTitleDone: Some required contents are missing");
    return ES_EINVAL;
  }

  if (!WriteImportTMD(import.tmd))
  {
    ERROR_LOG_FMT(IOS_ES, "ImportTitleDone: Failed to write import TMD");
    return ES_EIO;
  }
  if (!FinishImport(import.tmd))
  {
    ERROR_LOG_FMT(IOS_ES, "ImportTitleDone: Failed to finalise title import");
    return ES_EIO;
  }

  ResetTitleImportContext(&context, m_ios.GetIOSC());
  INFO_LOG_FMT(IOS_ES, "ImportTitleDone: title {:016x}", title_id);
  return IPC_SUCCESS;
}

IPCReply ESDevice::ImportTitleDone(Context& context, const IOCtlVRequest& request)
{
  const bool no_vectors = request.HasNumberOfValidVectors(0, 0);
  return IPCReply(no_vectors ? m_core.ImportTitleDone(context) : ES_EINVAL);
}

ReturnCode ESCore::ImportTitleCancel(Context& context)
{
  // A TMD may have been set even though the import never became valid.
  const auto& import = context.title_import_export;
  if (import.tmd.GetBytes().empty() || import.content.valid)
    return ES_EINVAL;

  if (import.valid)
  {
    const u64 title_id = import.tmd.GetTitleId();
    FinishStaleImport(title_id);
    INFO_LOG_FMT(IOS_ES, "ImportTitleCancel: title {:016x}", title_id);
  }

  ResetTitleImportContext(&context, m_ios.GetIOSC());
  return IPC_SUCCESS;
}

IPCReply ESDevice::ImportTitleCancel(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 0))
    return IPCReply(ES_EINVAL);
  return IPCReply(m_core.ImportTitleCancel(context));
}

// -----------------------------------------------------------------------------------------------
// Removal

ReturnCode ESCore::DeleteTitle(u64 title_id)
{
  if (!CanDeleteTitle(title_id))
    return ES_EINVAL;
  return FS::ConvertResult(
      m_ios.GetFS()->Delete(PID_KERNEL, PID_KERNEL, Common::GetTitlePath(title_id)));
}

IPCReply ESDevice::DeleteTitle(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0) || request.in_vectors[0].size != sizeof(u64))
    return IPCReply(ES_EINVAL);

  const u64 title_id = GetSystem().GetMemory().Read_U64(request.in_vectors[0].address);
  return IPCReply(m_core.DeleteTitle(title_id));
}

ReturnCode ESCore::DeleteTicket(const u8* ticket_view)
{
  const u64 title_id = Common::swap64(ticket_view + offsetof(ES::TicketView, title_id));
  if (!CanDeleteTitle(title_id))
    return ES_EINVAL;

  ES::TicketReader ticket = FindSignedTicket(title_id);
  if (!ticket.IsValid())
    return FS_ENOENT;

  const bool is_v1 = ticket.IsV1Ticket();
  const std::string path =
      is_v1 ? Common::GetV1TicketFileName(title_id) : Common::GetTicketFileName(title_id);

  // Remove the matching ticket(s) from the file; drop the file once nothing is left.
  ticket.DeleteTicket(Common::swap64(ticket_view + offsetof(ES::TicketView, ticket_id)));
  const std::vector<u8>& remaining = ticket.GetBytes();
  const auto fs = m_ios.GetFS();
  if (remaining.empty())
  {
    fs->Delete(PID_KERNEL, PID_KERNEL, path);
  }
  else
  {
    ASSERT(ticket.IsValid());
    ASSERT(ticket.IsV1Ticket() == is_v1);
    const auto file = fs->OpenFile(PID_KERNEL, PID_KERNEL, path, FS::Mode::ReadWrite);
    if (!file || !file->Write(remaining.data(), remaining.size()))
      return ES_EIO;
  }

  // Also drop /ticket/<high word> once it is empty.
  const std::string parent = fmt::format("/ticket/{:08x}", static_cast<u32>(title_id >> 32));
  const auto entries = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, parent);
  if (entries && entries->empty())
    fs->Delete(PID_KERNEL, PID_KERNEL, parent);

  return IPC_SUCCESS;
}

IPCReply ESDevice::DeleteTicket(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0) ||
      request.in_vectors[0].size != sizeof(ES::TicketView))
  {
    return IPCReply(ES_EINVAL);
  }

  const u8* view = GetSystem().GetMemory().GetPointerForRange(request.in_vectors[0].address,
                                                              sizeof(ES::TicketView));
  return IPCReply(m_core.DeleteTicket(view));
}

ReturnCode ESCore::DeleteTitleContent(u64 title_id) const
{
  if (!CanDeleteTitle(title_id))
    return ES_EINVAL;

  const auto fs = m_ios.GetFS();
  const std::string dir = Common::GetTitleContentPath(title_id);
  const auto names = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, dir);
  if (!names)
    return FS::ConvertResult(names.error());

  // Only content files (eight hex digits + ".app") are removed; the TMD stays.
  for (const std::string& name : *names)
  {
    if (name.size() == 12 && name.ends_with(".app"))
      fs->Delete(PID_KERNEL, PID_KERNEL, fmt::format("{}/{}", dir, name));
  }
  return IPC_SUCCESS;
}

IPCReply ESDevice::DeleteTitleContent(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0) || request.in_vectors[0].size != sizeof(u64))
    return IPCReply(ES_EINVAL);

  const u64 title_id = GetSystem().GetMemory().Read_U64(request.in_vectors[0].address);
  return IPCReply(m_core.DeleteTitleContent(title_id));
}

ReturnCode ESCore::DeleteContent(u64 title_id, u32 content_id) const
{
  if (!CanDeleteTitle(title_id))
    return ES_EINVAL;

  // The content has to be listed in the installed TMD.
  ES::Content listed;
  const ES::TMDReader tmd = FindInstalledTMD(title_id);
  if (!tmd.IsValid())
    return FS_ENOENT;
  if (!tmd.FindContentById(content_id, &listed))
    return ES_EINVAL;

  const std::string content_dir = Common::GetTitleContentPath(title_id);
  const FS::ResultCode result = m_ios.GetFS()->Delete(
      PID_KERNEL, PID_KERNEL, fmt::format("{}/{:08x}.app", content_dir, content_id));
  return FS::ConvertResult(result);
}

IPCReply ESDevice::DeleteContent(const IOCtlVRequest& request)
{
  // in: title ID, content ID
  const bool valid = request.HasNumberOfValidVectors(2, 0) &&
                     request.in_vectors[0].size == sizeof(u64) &&
                     request.in_vectors[1].size == sizeof(u32);
  if (!valid)
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const u32 content_id = memory.Read_U32(request.in_vectors[1].address);
  return IPCReply(m_core.DeleteContent(title_id, content_id));
}

ReturnCode ESCore::DeleteSharedContent(const std::array<u8, 20>& sha1) const
{
  ES::SharedContentMap map{m_ios.GetFSCore()};
  const std::optional<std::string> path = map.GetFilenameFromSHA1(sha1);
  if (!path)
    return ES_EINVAL;

  // Shared contents that a system title needs stay. A system title whose TMD cannot be read
  // counts as needing it.
  const auto needed_by = [&](u64 title_id) {
    if (!ES::IsTitleType(title_id, ES::TitleType::System))
      return false;
    const ES::TMDReader tmd = FindInstalledTMD(title_id);
    if (!tmd.IsValid())
      return true;
    for (const ES::Content& content : tmd.GetContents())
    {
      if (content.sha1 == sha1)
        return true;
    }
    return false;
  };
  for (const u64 title_id : GetInstalledTitles())
  {
    if (needed_by(title_id))
      return ES_EINVAL;
  }

  const FS::ResultCode deleted = m_ios.GetFS()->Delete(PID_KERNEL, PID_KERNEL, *path);
  if (deleted != FS::ResultCode::Success)
    return FS::ConvertResult(deleted);
  return map.DeleteSharedContent(sha1) ? IPC_SUCCESS : ES_EIO;
}

IPCReply ESDevice::DeleteSharedContent(const IOCtlVRequest& request)
{
  std::array<u8, 20> sha1;
  if (!request.HasNumberOfValidVectors(1, 0) || request.in_vectors[0].size != sha1.size())
    return IPCReply(ES_EINVAL);

  GetSystem().GetMemory().CopyFromEmu(sha1.data(), request.in_vectors[0].address, sha1.size());
  return IPCReply(m_core.DeleteSharedContent(sha1));
}

// -----------------------------------------------------------------------------------------------
// Title export

ReturnCode ESCore::ExportTitleInit(Context& context, u64 title_id, u8* tmd_bytes, u32 tmd_size,
                                   u64 caller_title_id, u32 caller_title_flags)
{
  // Only one import or export per handle.
  if (context.title_import_export.valid)
    return ES_EINVAL;

  const ES::TMDReader tmd = FindInstalledTMD(title_id);
  if (!tmd.IsValid())
    return FS_ENOENT;

  IOSC& iosc = m_ios.GetIOSC();
  ResetTitleImportContext(&context, iosc);
  auto& exp = context.title_import_export;
  exp.tmd = tmd;

  const ReturnCode ret = InitBackupKey(caller_title_id, caller_title_flags, iosc, &exp.key_handle);
  if (ret != IPC_SUCCESS)
    return ret;

  const std::vector<u8>& raw = exp.tmd.GetBytes();
  if (raw.size() != tmd_size)
    return ES_EINVAL;
  std::memcpy(tmd_bytes, raw.data(), raw.size());

  exp.valid = true;
  return IPC_SUCCESS;
}

IPCReply ESDevice::ExportTitleInit(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1) || request.in_vectors[0].size != sizeof(u64))
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const IOCtlVRequest::IOVector& out = request.io_vectors[0];
  u8* tmd_out = memory.GetPointerForRange(out.address, out.size);

  const ES::TMDReader& active_tmd = m_core.m_title_context.tmd;
  return IPCReply(m_core.ExportTitleInit(context, title_id, tmd_out, out.size,
                                         active_tmd.GetTitleId(), active_tmd.GetTitleFlags()));
}

ReturnCode ESCore::ExportContentBegin(Context& context, u64 title_id, u32 content_id)
{
  auto& exp = context.title_import_export;
  exp.content = {};
  if (!exp.valid || exp.tmd.GetTitleId() != title_id)
  {
    ERROR_LOG_FMT(IOS_ES, "Tried to use ExportContentBegin with an invalid title export context.");
    return ES_EINVAL;
  }

  ES::Content info;
  if (!exp.tmd.FindContentById(content_id, &info))
    return ES_EINVAL;

  exp.content.id = content_id;
  exp.content.valid = true;

  // The result is the content fd that the following export calls pass in.
  const s32 cfd = OpenContent(exp.tmd, info.index, 0);
  if (cfd < 0)
  {
    ResetTitleImportContext(&context, m_ios.GetIOSC());
    return static_cast<ReturnCode>(cfd);
  }
  SetContentIV(exp.content.iv, info.index);
  return static_cast<ReturnCode>(cfd);
}

IPCReply ESDevice::ExportContentBegin(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 0) || request.in_vectors[0].size != sizeof(u64) ||
      request.in_vectors[1].size != sizeof(u32))
  {
    return IPCReply(ES_EINVAL);
  }

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const u32 content_id = memory.Read_U32(request.in_vectors[1].address);
  return IPCReply(m_core.ExportContentBegin(context, title_id, content_id));
}

ReturnCode ESCore::ExportContentData(Context& context, u32 content_fd, u8* data, u32 data_size)
{
  auto& exp = context.title_import_export;
  if (!exp.valid || !exp.content.valid || data == nullptr || data_size == 0)
  {
    CloseContent(content_fd, 0);
    // Note: the key object is not freed on this path.
    exp = {};
    return ES_EINVAL;
  }

  std::vector<u8> chunk(data_size);
  if (ReadContent(content_fd, chunk.data(), data_size, 0) < 0)
  {
    CloseContent(content_fd, 0);
    ResetTitleImportContext(&context, m_ios.GetIOSC());
    return ES_SHORT_READ;
  }

  // IOS pads the chunk to 32 bytes (which also satisfies AES's 16) before encrypting it.
  chunk.resize(Common::AlignUp(chunk.size(), 32));
  std::vector<u8> encrypted(chunk.size());
  const ReturnCode ret = m_ios.GetIOSC().Encrypt(
      exp.key_handle, exp.content.iv.data(), chunk.data(), chunk.size(), encrypted.data(), PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;

  std::memcpy(data, encrypted.data(), encrypted.size());
  return IPC_SUCCESS;
}

IPCReply ESDevice::ExportContentData(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1) || request.in_vectors[0].size != sizeof(u32) ||
      request.io_vectors[0].size == 0)
  {
    return IPCReply(ES_EINVAL);
  }

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u32 content_fd = memory.Read_U32(request.in_vectors[0].address);
  const IOCtlVRequest::IOVector& out = request.io_vectors[0];
  u8* data = memory.GetPointerForRange(out.address, out.size);
  return IPCReply(m_core.ExportContentData(context, content_fd, data, out.size));
}

ReturnCode ESCore::ExportContentEnd(Context& context, u32 content_fd)
{
  const auto& exp = context.title_import_export;
  if (!exp.valid || !exp.content.valid)
    return ES_EINVAL;
  return static_cast<ReturnCode>(CloseContent(content_fd, 0));
}

IPCReply ESDevice::ExportContentEnd(Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0) || request.in_vectors[0].size != sizeof(u32))
    return IPCReply(ES_EINVAL);

  const u32 content_fd = GetSystem().GetMemory().Read_U32(request.in_vectors[0].address);
  return IPCReply(m_core.ExportContentEnd(context, content_fd));
}

ReturnCode ESCore::ExportTitleDone(Context& context)
{
  ResetTitleImportContext(&context, m_ios.GetIOSC());
  return IPC_SUCCESS;
}

// The request's vectors are not looked at.
IPCReply ESDevice::ExportTitleDone(Context& context, const IOCtlVRequest& /*request*/)
{
  return IPCReply(m_core.ExportTitleDone(context));
}
}  // namespace IOS::HLE
