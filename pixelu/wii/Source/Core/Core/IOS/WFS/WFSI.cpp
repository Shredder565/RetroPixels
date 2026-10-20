// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// WFS title installer. A title is imported as encrypted U8 archives (contents and profiles)
// that get unpacked into the WFS volume:
//   /vol/<device>/_install/<title>/...        a new title while it is being installed
//   /vol/<device>/title/<group>/<title>/...   installed titles (and _patch/ for updates)
//   /vol/<device>/tmp/                        profile files

#include "Core/IOS/WFS/WFSI.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Crypto/AES.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/WFS/WFSSRV.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
// Title and group IDs appear in paths as their ASCII characters (title: the low 32 bits).
std::string TitleIdStr(u64 tid)
{
  std::string s(4, '\0');
  for (size_t i = 0; i < s.size(); ++i)
    s[i] = static_cast<char>(tid >> (24 - 8 * i));
  return s;
}

std::string GroupIdStr(u16 gid)
{
  return {static_cast<char>(gid >> 8), static_cast<char>(gid)};
}

std::string TitleDir(const std::string& device, const std::string& group, const std::string& title)
{
  return fmt::format("/vol/{}/title/{}/{}", device, group, title);
}

std::string TmdFileName(u64 title_id)
{
  return fmt::format("/meta/{:016x}.tmd", title_id);
}

void DeleteTemporaryFiles(const std::string& device, u64 title_id)
{
  for (const char* extension : {"ini", "ppcini"})
  {
    File::Delete(
        WFS::NativePath(fmt::format("/vol/{}/tmp/{:016x}.{}", device, title_id, extension)));
  }
}

constexpr s32 WFSI_ENOTICKET = -11028;
constexpr s32 WFSI_EPERMISSION = -3;
}  // namespace

void ARCUnpacker::Reset()
{
  m_whole_file.clear();
}

void ARCUnpacker::AddBytes(std::span<const u8> bytes)
{
  m_whole_file.insert(m_whole_file.end(), bytes.begin(), bytes.end());
}

void ARCUnpacker::Extract(const WriteCallback& callback)
{
  // U8 archive: a header, then a flat node table where each directory node gives the index of
  // the first node after its contents. Node 0 is the root, and its end index is the node count.
  constexpr u32 U8_MAGIC = 0x55AA382D;
  constexpr u32 NODE_TABLE_OFFSET = 0x20;
  constexpr u32 NODE_SIZE = 0xC;

  const u8* const archive = m_whole_file.data();
  const u32 magic = Common::swap32(archive);
  if (magic != U8_MAGIC)
  {
    ERROR_LOG_FMT(IOS_WFS, "ARCUnpacker: bad magic {:08x}", magic);
    return;
  }

  const u8* const nodes = archive + NODE_TABLE_OFFSET;
  const u32 node_count = Common::swap32(nodes + 8);
  const char* const names = reinterpret_cast<const char*>(nodes + node_count * NODE_SIZE);

  struct OpenDirectory
  {
    u32 end;
    std::string path;
  };
  std::vector<OpenDirectory> open_dirs{{node_count, {}}};

  for (u32 index = 1; index < node_count; ++index)
  {
    while (index >= open_dirs.back().end)
      open_dirs.pop_back();

    // type (1 = directory), 24-bit name offset, data offset / parent, size / end index
    const u8* const node = nodes + index * NODE_SIZE;
    const u32 name_offset = (u32{node[1]} << 16) | Common::swap16(node + 2);
    const u32 field1 = Common::swap32(node + 4);
    const u32 field2 = Common::swap32(node + 8);

    const std::string& parent = open_dirs.back().path;
    std::string path(names + name_offset);
    if (!parent.empty())
      path = parent + '/' + path;

    if (node[0] == 1)
      open_dirs.push_back({field2, std::move(path)});
    else
      callback(path, std::span<const u8>(archive + field1, field2));
  }
}

WFSIDevice::WFSIDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
}

void WFSIDevice::SetCurrentTitleIdAndGroupId(u64 tid, u16 gid)
{
  m_current_title_id = tid;
  m_current_title_id_str = TitleIdStr(tid);
  m_current_group_id = gid;
  m_current_group_id_str = GroupIdStr(gid);
}

void WFSIDevice::SetImportTitleIdAndGroupId(u64 tid, u16 gid)
{
  m_import_title_id = tid;
  m_import_title_id_str = TitleIdStr(tid);
  m_import_group_id = gid;
  m_import_group_id_str = GroupIdStr(gid);
}

void WFSIDevice::FinalizePatchInstall()
{
  // The patch directory's contents replace the title's.
  const std::string title_dir =
      TitleDir(m_device_name, m_current_group_id_str, m_current_title_id_str);
  File::MoveWithOverwrite(WFS::NativePath(title_dir + "/_patch"), WFS::NativePath(title_dir));
}

std::optional<IPCReply> WFSIDevice::IOCtl(const IOCtlRequest& request)
{
  Core::System& system = GetSystem();
  Memory::MemoryManager& memory = system.GetMemory();
  const u32 in = request.buffer_in;
  const u32 out = request.buffer_out;
  s32 result = IPC_SUCCESS;

  // The running title's directory.
  const auto current_dir = [this] {
    return TitleDir(m_device_name, m_current_group_id_str, m_current_title_id_str);
  };

  switch (request.request)
  {
  case IOCTL_WFSI_IMPORT_TITLE_INIT:
  {
    // in: TMD address, TMD size, ..., patch type (+32), continue flag (+36)
    const u32 tmd_address = memory.Read_U32(in);
    const u32 tmd_size = memory.Read_U32(in + 4);
    m_patch_type = static_cast<PatchType>(memory.Read_U32(in + 32));
    m_continue_install = memory.Read_U32(in + 36) != 0;
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFSI_IMPORT_TITLE_INIT: patch type {}, continue {}",
                 std::to_underlying(m_patch_type), m_continue_install);

    // Type 2 patches replace the main executable; keep the old one aside.
    if (m_patch_type == PATCH_TYPE_2)
    {
      const std::string content = current_dir() + "/content";
      File::Rename(WFS::NativePath(content + "/default.dol"),
                   WFS::NativePath(content + "/_default.dol"));
    }

    if (!ES::IsValidTMDSize(tmd_size))
    {
      ERROR_LOG_FMT(IOS_WFS, "IOCTL_WFSI_IMPORT_TITLE_INIT: bad TMD size {}", tmd_size);
      result = IPC_EINVAL;
      break;
    }
    std::vector<u8> tmd(tmd_size);
    memory.CopyFromEmu(tmd.data(), tmd_address, tmd_size);
    m_tmd.SetBytes(std::move(tmd));

    // The contents are encrypted with the title key from the installed ticket.
    const ES::TicketReader ticket =
        GetEmulationKernel().GetESCore().FindSignedTicket(m_tmd.GetTitleId());
    if (!ticket.IsValid())
    {
      result = WFSI_ENOTICKET;
      break;
    }
    m_aes_ctx = Common::AES::CreateContextDecrypt(ticket.GetTitleKey(m_ios.GetIOSC()).data());

    SetImportTitleIdAndGroupId(m_tmd.GetTitleId(), m_tmd.GetGroupId());
    if (m_patch_type == PATCH_TYPE_1)
      CancelPatchImport(m_continue_install);
    else if (m_patch_type == NOT_A_PATCH)
      CancelTitleImport(m_continue_install);
    break;
  }

  case IOCTL_WFSI_PREPARE_PROFILE:
  case IOCTL_WFSI_PREPARE_CONTENT:
  {
    const bool profile = request.request == IOCTL_WFSI_PREPARE_PROFILE;
    if (profile)
      m_base_extract_path = fmt::format("/vol/{}/tmp/", m_device_name);

    // The IV is the content's index in the TMD.
    const u32 content_id = memory.Read_U32(in + 8);
    ES::Content content;
    if (!m_tmd.FindContentById(content_id, &content))
    {
      WARN_LOG_FMT(IOS_WFS, "{}: no content {:08x} in the TMD",
                   profile ? "IOCTL_WFSI_PREPARE_PROFILE" : "IOCTL_WFSI_PREPARE_CONTENT",
                   content_id);
      result = WFS_EINVAL;
      break;
    }
    std::fill(std::begin(m_aes_iv), std::end(m_aes_iv), u8{0});
    m_aes_iv[0] = static_cast<u8>(content.index >> 8);
    m_aes_iv[1] = static_cast<u8>(content.index);
    INFO_LOG_FMT(IOS_WFS, "Prepare content {:08x} (index {})", content_id, content.index);

    m_arc_unpacker.Reset();
    break;
  }

  case IOCTL_WFSI_IMPORT_PROFILE:
  case IOCTL_WFSI_IMPORT_CONTENT:
  {
    // in: ..., content ID (+0xC), data address (+0x10), data size (+0x14)
    const u32 content_id = memory.Read_U32(in + 0xC);
    const u32 data_address = memory.Read_U32(in + 0x10);
    const u32 data_size = memory.Read_U32(in + 0x14);
    INFO_LOG_FMT(IOS_WFS, "Import {:#x} bytes at {:08x} of content {:08x} (ioctl {:#x})", data_size,
                 data_address, content_id, request.request);

    // Chunks are decrypted in sequence, so the IV carries over.
    std::vector<u8> plain(data_size);
    m_aes_ctx->Crypt(m_aes_iv, m_aes_iv, memory.GetPointerForRange(data_address, data_size),
                     plain.data(), data_size);
    m_arc_unpacker.AddBytes(plain);
    break;
  }

  case IOCTL_WFSI_IMPORT_CONTENT_END:
  case IOCTL_WFSI_IMPORT_PROFILE_END:
  {
    INFO_LOG_FMT(IOS_WFS, "Import end (ioctl {:#x})", request.request);
    m_arc_unpacker.Extract([this](const std::string& name, std::span<const u8> bytes) {
      const std::string host_path = WFS::NativePath(m_base_extract_path + '/' + name);
      INFO_LOG_FMT(IOS_WFS, "Extracting {} ({} bytes)", name, bytes.size());
      File::CreateFullPath(host_path);
      File::IOFile file(host_path, "wb");
      if (file)
        file.WriteBytes(bytes.data(), bytes.size());
      else
        ERROR_LOG_FMT(IOS_WFS, "Cannot write {} to {}", name, host_path);
    });
    // Free the archive now rather than at the next import.
    m_arc_unpacker.Reset();
    break;
  }

  case IOCTL_WFSI_FINALIZE_TITLE_INSTALL:
  {
    std::string tmd_path;
    if (m_patch_type == NOT_A_PATCH)
    {
      // Move the new title into place.
      const std::string staging =
          fmt::format("/vol/{}/_install/{}", m_device_name, m_import_title_id_str);
      const std::string final_dir =
          TitleDir(m_device_name, m_import_group_id_str, m_import_title_id_str);
      File::Rename(WFS::NativePath(staging), WFS::NativePath(final_dir));
      tmd_path = final_dir + TmdFileName(m_import_title_id);
    }
    else if (m_patch_type == PATCH_TYPE_1 || m_patch_type == PATCH_TYPE_2)
    {
      const std::string title_dir = current_dir();
      if (m_patch_type == PATCH_TYPE_2)
      {
        // The kept-aside executable moves into the patch before the patch is applied.
        const std::string content = title_dir + "/content/";
        const std::string patch = title_dir + "/_patch/";
        const std::string patch_content = patch + "content/";
        File::Delete(WFS::NativePath(content + "default.dol"));
        File::CreateDir(WFS::NativePath(patch));
        File::CreateDir(WFS::NativePath(patch_content));
        File::Rename(WFS::NativePath(content + "_default.dol"),
                     WFS::NativePath(patch_content + "default.dol"));
        FinalizePatchInstall();
      }
      File::DeleteDirRecursively(WFS::NativePath(title_dir + "/_patch"));
      tmd_path = title_dir + TmdFileName(m_import_title_id);
    }

    // For an unknown patch type the path stays empty and the write simply fails.
    File::IOFile tmd_file(WFS::NativePath(tmd_path), "wb");
    const std::vector<u8>& tmd = m_tmd.GetBytes();
    tmd_file.WriteBytes(tmd.data(), tmd.size());
    break;
  }

  case IOCTL_WFSI_FINALIZE_PATCH_INSTALL:
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFSI_FINALIZE_PATCH_INSTALL");
    if (m_patch_type != NOT_A_PATCH)
      FinalizePatchInstall();
    break;

  case IOCTL_WFSI_DELETE_TITLE:
    WARN_LOG_FMT(IOS_WFS, "IOCTL_WFSI_DELETE_TITLE is not implemented");
    break;

  case IOCTL_WFSI_CHANGE_TITLE:
  {
    // in: title ID (+0), group ID (+0x1C). Permissions are not checked.
    const u64 title_id = memory.Read_U64(in);
    const u16 group_id = memory.Read_U16(in + 0x1C);
    SetCurrentTitleIdAndGroupId(title_id, group_id);

    // The new title's directory becomes the WFS home directory.
    const std::string home = current_dir() + '/';
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFSI_CHANGE_TITLE: home is now {} ({:#x} chars)", home,
                 home.size());

    result = WFSI_EPERMISSION;
    if (static_cast<u16>(home.size()) > 0x1FD)
      break;
    const std::shared_ptr<Device> wfssrv = GetEmulationKernel().GetDeviceByName("/dev/usb/wfssrv");
    if (!wfssrv)
      break;
    std::static_pointer_cast<WFSSRVDevice>(wfssrv)->SetHomeDir(home);
    result = IPC_SUCCESS;
    break;
  }

  case IOCTL_WFSI_GET_VERSION:
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFSI_GET_VERSION");
    memory.Write_U32(0x20, out);
    break;

  case IOCTL_WFSI_IMPORT_TITLE_CANCEL:
  {
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFSI_IMPORT_TITLE_CANCEL");
    const bool keep_files = memory.Read_U32(in) != 0;
    switch (m_patch_type)
    {
    case NOT_A_PATCH:
      result = CancelTitleImport(keep_files);
      break;
    case PATCH_TYPE_1:
    case PATCH_TYPE_2:
      result = CancelPatchImport(keep_files);
      break;
    default:
      result = WFS_EINVAL;
      break;
    }
    m_tmd = {};
    break;
  }

  case IOCTL_WFSI_INIT:
  {
    // The running title (from ES) becomes the current title.
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFSI_INIT");
    ESCore& es = GetEmulationKernel().GetESCore();
    u64 title_id;
    if (es.GetTitleId(&title_id) < 0)
    {
      ERROR_LOG_FMT(IOS_WFS, "IOCTL_WFSI_INIT: no running title");
      result = IPC_EINVAL;
      break;
    }
    const ES::TMDReader tmd = es.FindInstalledTMD(title_id);
    SetCurrentTitleIdAndGroupId(tmd.GetTitleId(), tmd.GetGroupId());
    break;
  }

  case IOCTL_WFSI_SET_DEVICE_NAME:
    m_device_name = memory.GetString(in);
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFSI_SET_DEVICE_NAME: {}", m_device_name);
    break;

  case IOCTL_WFSI_APPLY_TITLE_PROFILE:
  {
    // Sets up the directories that the contents are extracted into.
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFSI_APPLY_TITLE_PROFILE");
    const auto make_dir = [](const std::string& path, bool with_parents) {
      if (with_parents)
        File::CreateFullPath(WFS::NativePath(path));
      File::CreateDir(WFS::NativePath(path));
    };

    if (m_patch_type != NOT_A_PATCH)
    {
      m_base_extract_path = current_dir() + "/_patch/content";
      make_dir(m_base_extract_path, true);
      break;
    }

    const std::string staging_root = fmt::format("/vol/{}/_install", m_device_name);
    if (!m_continue_install && File::IsDirectory(WFS::NativePath(staging_root)))
      File::DeleteDirRecursively(WFS::NativePath(staging_root));

    const std::string staging = staging_root + '/' + m_import_title_id_str;
    m_base_extract_path = staging + "/content";
    make_dir(m_base_extract_path, true);
    for (const char* sub : {"work", "meta", "save"})
      make_dir(staging + '/' + sub, false);
    make_dir(fmt::format("/vol/{}/title/{}", m_device_name, m_import_group_id_str), true);
    break;
  }

  case IOCTL_WFSI_GET_TMD:
  case IOCTL_WFSI_GET_TMD_ABSOLUTE:
  {
    // in: subtitle ID (+0), output address (+24); the absolute variant also names the title
    // (+32) and group (+36) instead of using the current ones.
    const u64 subtitle_id = memory.Read_U64(in);
    const u32 address = memory.Read_U32(in + 24);
    u32 title_id = static_cast<u32>(m_current_title_id);
    u16 group_id = m_current_group_id;
    if (request.request == IOCTL_WFSI_GET_TMD_ABSOLUTE)
    {
      group_id = memory.Read_U16(in + 36);
      title_id = memory.Read_U32(in + 32);
    }
    INFO_LOG_FMT(IOS_WFS, "Get TMD: title {:08x}, group {:04x}, subtitle {:016x}", title_id,
                 group_id, subtitle_id);

    u32 size = 0;
    result = static_cast<s32>(GetTmd(group_id, title_id, subtitle_id, address, &size));
    memory.Write_U32(size, out);
    break;
  }

  case IOCTL_WFSI_SET_FST_BUFFER:
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFSI_SET_FST_BUFFER: {:08x}, {:#x} bytes", in,
                 request.buffer_in_size);
    break;

  case IOCTL_WFSI_NOOP:
    break;

  case IOCTL_WFSI_LOAD_DOL:
  {
    // in: ..., buffer size (+0x14), buffer address (+0x18), extension number (+0x1e)
    const u32 max_size = memory.Read_U32(in + 0x14);
    const u32 address = memory.Read_U32(in + 0x18);
    const u16 extension = memory.Read_U16(in + 0x1e);
    const std::string path =
        current_dir() + "/content" +
        (extension == 0 ? std::string("/default.dol") : fmt::format("/extension{}.dol", extension));
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFSI_LOAD_DOL: {} to {:08x} ({:#x} bytes)", path, address,
                 max_size);

    File::IOFile dol(WFS::NativePath(path), "rb");
    if (!dol)
    {
      WARN_LOG_FMT(IOS_WFS, "IOCTL_WFSI_LOAD_DOL: {} not found", path);
      result = WFS_ENOENT;
      break;
    }

    // A null address asks for the size only, which is also written back into the request.
    const auto dol_size = static_cast<u32>(dol.GetSize());
    if (address == 0)
      memory.Write_U32(dol_size, in + 0x14);
    else
      dol.ReadBytes(memory.GetPointerForRange(address, max_size), max_size);
    memory.Write_U32(dol_size, out);
    break;
  }

  case IOCTL_WFSI_CHECK_HAS_SPACE:
    // Not emulated: always report enough free space (1). IOS would ask WFSSRV for the free
    // blocks of the destination volume.
    WARN_LOG_FMT(IOS_WFS, "IOCTL_WFSI_CHECK_HAS_SPACE: reporting free space");
    result = 1;
    break;

  default:
    // Pretend unknown requests worked, as with WFSSRV.
    request.DumpUnknown(system, GetDeviceName(), Common::Log::LogType::IOS_WFS,
                        Common::Log::LogLevel::LWARNING);
    memory.Memset(out, 0, request.buffer_out_size);
    break;
  }

  return IPCReply(result);
}

u32 WFSIDevice::GetTmd(u16 group_id, u32 title_id, u64 subtitle_id, u32 address, u32* size) const
{
  const std::string path = TitleDir(m_device_name, GroupIdStr(group_id), TitleIdStr(title_id)) +
                           TmdFileName(subtitle_id);
  File::IOFile tmd(WFS::NativePath(path), "rb");
  if (!tmd)
  {
    WARN_LOG_FMT(IOS_WFS, "GetTmd: {} not found", path);
    return static_cast<u32>(WFS_ENOENT);
  }

  *size = static_cast<u32>(tmd.GetSize());
  if (address != 0)
    tmd.ReadBytes(GetSystem().GetMemory().GetPointerForRange(address, *size), *size);
  return IPC_SUCCESS;
}

s32 WFSIDevice::CancelTitleImport(bool continue_install)
{
  m_arc_unpacker.Reset();
  if (!continue_install)
    File::DeleteDirRecursively(WFS::NativePath(fmt::format("/vol/{}/_install", m_device_name)));
  DeleteTemporaryFiles(m_device_name, m_import_title_id);
  return IPC_SUCCESS;
}

s32 WFSIDevice::CancelPatchImport(bool continue_install)
{
  m_arc_unpacker.Reset();
  if (!continue_install)
  {
    const std::string title_dir =
        TitleDir(m_device_name, m_current_group_id_str, m_current_title_id_str);
    File::DeleteDirRecursively(WFS::NativePath(title_dir + "/_patch"));

    // Undo the executable swap of IMPORT_TITLE_INIT.
    if (m_patch_type == PATCH_TYPE_2)
    {
      const std::string content = title_dir + "/content";
      File::Rename(WFS::NativePath(content + "/_default.dol"),
                   WFS::NativePath(content + "/default.dol"));
    }
  }
  DeleteTemporaryFiles(m_device_name, m_current_title_id);
  return IPC_SUCCESS;
}
}  // namespace IOS::HLE
