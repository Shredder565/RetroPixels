// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/ES/ES.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Core/AchievementManager.h"
#include "Core/CommonTitles.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/FS/FileSystemProxy.h"
#include "Core/IOS/IOSC.h"
#include "Core/IOS/Uids.h"
#include "Core/IOS/VersionInfo.h"
#include "Core/System.h"
#include "DiscIO/Enums.h"

namespace IOS::HLE
{
namespace
{
using FS::Mode;

// Directories ES makes sure exist (with these owners and permissions) whenever it starts.
struct StandardDirectory
{
  const char* path;
  FS::Modes modes;
  FS::Uid owner;
  FS::Gid group;
};

constexpr FS::Modes SYSTEM_ONLY{Mode::ReadWrite, Mode::ReadWrite, Mode::None};
constexpr FS::Modes WORLD_READABLE{Mode::ReadWrite, Mode::ReadWrite, Mode::Read};
constexpr FS::Modes WORLD_WRITABLE{Mode::ReadWrite, Mode::ReadWrite, Mode::ReadWrite};

constexpr std::array<StandardDirectory, 9> STANDARD_DIRECTORIES{{
    {"/sys", SYSTEM_ONLY, PID_KERNEL, PID_KERNEL},
    {"/ticket", SYSTEM_ONLY, PID_KERNEL, PID_KERNEL},
    {"/title", WORLD_READABLE, PID_KERNEL, PID_KERNEL},
    {"/shared1", SYSTEM_ONLY, PID_KERNEL, PID_KERNEL},
    {"/shared2", WORLD_WRITABLE, PID_KERNEL, PID_KERNEL},
    {"/tmp", WORLD_WRITABLE, PID_KERNEL, PID_KERNEL},
    {"/import", SYSTEM_ONLY, PID_KERNEL, PID_KERNEL},
    {"/meta", WORLD_WRITABLE, SYSMENU_UID, SYSMENU_GID},
    {"/wfs", {Mode::ReadWrite, Mode::None, Mode::None}, PID_UNKNOWN, PID_UNKNOWN},
}};

// Present while a PPC title launch is waiting for the IOS reload it asked for.
constexpr char LAUNCH_FILE_PATH[] = "/sys/launch.sys";
// Space reserved after a launch (a title ID, a ticket view and a maximum-size TMD).
constexpr char SPACE_FILE_PATH[] = "/sys/space.sys";
constexpr size_t SPACE_FILE_SIZE = sizeof(u64) + sizeof(ES::TicketView) + ES::MAX_TMD_SIZE;

constexpr char CERT_STORE_PATH[] = "/sys/cert.sys";

CoreTiming::EventType* s_finish_init_event;
CoreTiming::EventType* s_reload_ios_for_ppc_launch_event;
CoreTiming::EventType* s_bootstrap_ppc_for_launch_event;

// How long ES takes to come up after an IOS boot, by IOS version.
constexpr SystemTimers::TimeBaseTick GetESBootTicks(u32 ios_version)
{
  u64 tb_ticks;
  if (ios_version < 28)
    tb_ticks = 22'000'000;
  else if (ios_version < 57)  // from IOS28 on, ES loads the other modules itself
    tb_ticks = 33'000'000;
  else if (ios_version <= 59)  // IOS57-59 carry extra modules
    tb_ticks = 39'000'000;
  else
    tb_ticks = 37'000'000;
  return SystemTimers::TimeBaseTick(tb_ticks);
}

bool UpdateUIDAndGID(EmulationKernel& kernel, ES::UIDSys* uid_sys, const ES::TMDReader& tmd)
{
  // UID 0 means uid.sys could not be updated.
  const u64 title_id = tmd.GetTitleId();
  if (const u32 uid = uid_sys->GetOrInsertUIDForTitle(title_id); uid != 0)
  {
    kernel.SetUidForPPC(uid);
    kernel.SetGidForPPC(tmd.GetGroupId());
    return true;
  }
  ERROR_LOG_FMT(IOS_ES, "Failed to get UID for title {:016x}", title_id);
  return false;
}

// Data transfer titles that IOS62 lets change the UID besides the system menu. The low byte of
// the title ID (the region) is not checked.
constexpr std::array<u64, 3> IOS62_SETUID_TITLES{
    0x00010001'484353ff,
    0x00010001'484354ff,
    0x00010008'48435aff,
};

ReturnCode CheckIsAllowedToSetUID(EmulationKernel& kernel, ES::UIDSys* uid_sys,
                                  const u32 caller_uid, const ES::TMDReader& active_tmd)
{
  const u32 system_menu_uid = uid_sys->GetOrInsertUIDForTitle(Titles::SYSTEM_MENU);
  if (system_menu_uid == 0)
    return ES_SHORT_READ;
  if (caller_uid == system_menu_uid)
    return IPC_SUCCESS;

  if (kernel.GetVersion() == 62 && active_tmd.IsValid())
  {
    const u64 masked = active_tmd.GetTitleId() | 0xff;
    if (std::ranges::find(IOS62_SETUID_TITLES, masked) != IOS62_SETUID_TITLES.end())
      return IPC_SUCCESS;
  }
  return ES_EINVAL;
}

ReturnCode WriteTmdForDiVerify(FS::FileSystem* fs, const ES::TMDReader& tmd)
{
  const std::string staging = "/tmp/title.tmd";
  fs->Delete(PID_KERNEL, PID_KERNEL, staging);
  {
    const auto file = fs->CreateAndOpenFile(PID_KERNEL, PID_KERNEL, staging, SYSTEM_ONLY);
    if (!file)
      return FS::ConvertResult(file.error());
    const std::vector<u8>& bytes = tmd.GetBytes();
    if (!file->Write(bytes.data(), bytes.size()))
      return ES_EIO;
  }

  const u64 title_id = tmd.GetTitleId();
  const std::string content_dir = Common::GetTitleContentPath(title_id);
  const std::string tmd_path = Common::GetTMDFileName(title_id);
  const FS::ResultCode created =
      fs->CreateFullPath(PID_KERNEL, PID_KERNEL, tmd_path, 0, WORLD_READABLE);
  if (created != FS::ResultCode::Success)
    return FS::ConvertResult(created);

  fs->SetMetadata(PID_KERNEL, content_dir, PID_KERNEL, PID_KERNEL, 0, SYSTEM_ONLY);
  return FS::ConvertResult(fs->Rename(PID_KERNEL, PID_KERNEL, staging, tmd_path));
}
}  // namespace

// -----------------------------------------------------------------------------------------------
// Start-up

ESCore::ESCore(Kernel& ios) : m_ios(ios)
{
  // ES runs its file system accesses as UID/GID 0 (not as PID 1).
  const auto fs = m_ios.GetFS();
  for (const StandardDirectory& dir : STANDARD_DIRECTORIES)
  {
    const FS::ResultCode result =
        fs->CreateDirectory(PID_KERNEL, PID_KERNEL, dir.path, 0, dir.modes);
    if (result != FS::ResultCode::Success && result != FS::ResultCode::AlreadyExists)
    {
      ERROR_LOG_FMT(IOS_ES, "Failed to create {}: error {}", dir.path,
                    std::to_underlying(FS::ConvertResult(result)));
    }
    // Existing directories get their owner and permissions reset too.
    fs->SetMetadata(0, dir.path, dir.owner, dir.group, 0, dir.modes);
  }

  FinishAllStaleImports();
}

ESCore::~ESCore() = default;

ESDevice::ESDevice(EmulationKernel& ios, ESCore& core, const std::string& device_name)
    : EmulationDevice(ios, device_name), m_core(core)
{
  Core::System& system = ios.GetSystem();
  if (!Core::IsRunning(system))
  {
    FinishInit();
    return;
  }
  CoreTiming::CoreTimingManager& core_timing = system.GetCoreTiming();
  core_timing.RemoveEvent(s_finish_init_event);
  core_timing.ScheduleEvent(static_cast<s64>(u64{GetESBootTicks(ios.GetVersion())}),
                            s_finish_init_event);
}

ESDevice::~ESDevice() = default;

void ESDevice::InitializeEmulationState(CoreTiming::CoreTimingManager& core_timing)
{
  const auto es = [](Core::System& system) { return system.GetIOS()->GetESDevice(); };

  s_finish_init_event = core_timing.RegisterEvent(
      "IOS-ESFinishInit", [es](Core::System& system, u64, s64) { es(system)->FinishInit(); });
  s_reload_ios_for_ppc_launch_event = core_timing.RegisterEvent(
      "IOS-ESReloadIOSForPPCLaunch", [es](Core::System& system, u64 ios_id, s64) {
        es(system)->LaunchTitle(ios_id, HangPPC::Yes);
      });
  s_bootstrap_ppc_for_launch_event = core_timing.RegisterEvent(
      "IOS-ESBootstrapPPCForLaunch",
      [es](Core::System& system, u64, s64) { es(system)->BootstrapPPC(); });
}

void ESDevice::FinalizeEmulationState()
{
  s_finish_init_event = nullptr;
  s_reload_ios_for_ppc_launch_event = nullptr;
  s_bootstrap_ppc_for_launch_event = nullptr;
}

void ESDevice::FinishInit()
{
  EmulationKernel& kernel = GetEmulationKernel();
  kernel.InitIPC();

  // A launch file left behind means we are the IOS that a PPC title launch reloaded into.
  std::optional<u64> relaunch_title;
  if (const auto file =
          kernel.GetFS()->OpenFile(PID_KERNEL, PID_KERNEL, LAUNCH_FILE_PATH, FS::Mode::Read))
  {
    u64 stored_title_id;
    if (file->Read(&stored_title_id, 1).has_value())
      relaunch_title = stored_title_id;
  }

  if (!relaunch_title)
    return;
  NOTICE_LOG_FMT(IOS, "Re-launching title {:016x} after IOS reload.", *relaunch_title);
  LaunchTitle(*relaunch_title, HangPPC::No);
}

// -----------------------------------------------------------------------------------------------
// Title context

void TitleContext::Clear()
{
  tmd = {};
  ticket = {};
  active = false;
}

void TitleContext::Update(const ES::TMDReader& tmd_, const ES::TicketReader& ticket_,
                          DiscIO::Platform platform)
{
  if (!tmd_.IsValid() || !ticket_.IsValid())
  {
    ERROR_LOG_FMT(IOS_ES, "TMD or ticket is not valid -- refusing to update title context");
    return;
  }

  tmd = tmd_;
  ticket = ticket_;
  active = true;

  // Only the first title of a session is reported; later changes all follow an IOS reload,
  // which creates a fresh context.
  if (!first_change)
    return;
  SConfig& config = SConfig::GetInstance();
  config.SetRunningGameMetadata(tmd, platform);
  config.OnESTitleChanged();
  first_change = false;
}

// Saved: ticket, TMD, active flag.
void TitleContext::DoState(PointerWrap& p)
{
  ticket.DoState(p);
  tmd.DoState(p);
  p.Do(active);
}

// -----------------------------------------------------------------------------------------------
// Simple queries

IPCReply ESDevice::GetTitleDirectory(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 1))
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  const std::string dir = fmt::format("/title/{:08x}/{:08x}/data", static_cast<u32>(title_id >> 32),
                                      static_cast<u32>(title_id));
  // Only the characters are copied; the caller's buffer provides the terminator.
  memory.CopyToEmu(request.io_vectors[0].address, dir.data(), dir.size());

  INFO_LOG_FMT(IOS_ES, "IOCTL_ES_GETTITLEDIR: {}", dir);
  return IPCReply(IPC_SUCCESS);
}

ReturnCode ESCore::GetTitleId(u64* title_id) const
{
  if (m_title_context.active)
  {
    *title_id = m_title_context.tmd.GetTitleId();
    return IPC_SUCCESS;
  }
  return ES_EINVAL;
}

IPCReply ESDevice::GetTitleId(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1))
    return IPCReply(ES_EINVAL);

  u64 title_id;
  if (const ReturnCode ret = m_core.GetTitleId(&title_id); ret != IPC_SUCCESS)
    return IPCReply(ret);

  GetSystem().GetMemory().Write_U64(title_id, request.io_vectors[0].address);
  INFO_LOG_FMT(IOS_ES, "IOCTL_ES_GETTITLEID: {:08x}/{:08x}", static_cast<u32>(title_id >> 32),
               static_cast<u32>(title_id));
  return IPCReply(IPC_SUCCESS);
}

IPCReply ESDevice::SetUID(u32 uid, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0) || request.in_vectors[0].size != sizeof(u64))
    return IPCReply(ES_EINVAL);

  const u64 title_id = GetSystem().GetMemory().Read_U64(request.in_vectors[0].address);

  EmulationKernel& kernel = GetEmulationKernel();
  ES::UIDSys uid_sys{kernel.GetFSCore()};
  const ReturnCode permission =
      CheckIsAllowedToSetUID(kernel, &uid_sys, uid, m_core.m_title_context.tmd);
  if (permission < 0)
  {
    ERROR_LOG_FMT(IOS_ES, "SetUID: Permission check failed with error {}",
                  static_cast<s32>(permission));
    return IPCReply(permission);
  }

  // The reply delay is whatever looking up the TMD costs. That is a rough figure, but the
  // default delay is far too short for the work involved.
  u64 delay = 0;
  const ES::TMDReader tmd = m_core.FindInstalledTMD(title_id, Ticks{&delay});
  if (!tmd.IsValid())
    return IPCReply(FS_ENOENT, delay);

  if (!UpdateUIDAndGID(kernel, &uid_sys, tmd))
  {
    ERROR_LOG_FMT(IOS_ES, "SetUID: Failed to get UID for title {:016x}", title_id);
    return IPCReply(ES_SHORT_READ, delay);
  }
  return IPCReply(IPC_SUCCESS, delay);
}

IPCReply ESDevice::GetConsumption(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 2))
    return IPCReply(ES_EINVAL);

  // No consumption records are kept: always report zero entries.
  GetSystem().GetMemory().Write_U32(0, request.io_vectors[1].address);
  INFO_LOG_FMT(IOS_ES, "IOCTL_ES_GETCONSUMPTION");
  return IPCReply(IPC_SUCCESS);
}

// -----------------------------------------------------------------------------------------------
// Launching

bool ESDevice::LaunchTitle(u64 title_id, HangPPC hang_ppc)
{
  m_core.m_title_context.Clear();
  INFO_LOG_FMT(IOS_ES, "ES_Launch: Title context changed: (none)");

  NOTICE_LOG_FMT(IOS_ES, "Launching title {:016x}...", title_id);

  const bool is_shop = title_id == Titles::SHOP || title_id == Titles::KOREAN_SHOP;
  if (is_shop && GetEmulationKernel().GetIOSC().IsUsingDefaultId())
  {
    ERROR_LOG_FMT(IOS_ES, "Refusing to launch the shop channel with default device credentials");
    CriticalAlertFmtT(
        "You cannot use the Wii Shop Channel without using your own device credentials."
        "\nPlease refer to the NAND usage guide for setup instructions: "
        "https://dolphin-emu.org/docs/guides/nand-usage-guide/");
    // Fall back to the system menu (which owners of the shop always have) rather than failing,
    // as an error here would likely crash the menu.
    return LaunchTitle(Titles::SYSTEM_MENU, hang_ppc);
  }

  const bool is_ios =
      IsTitleType(title_id, ES::TitleType::System) && title_id != Titles::SYSTEM_MENU;
  return is_ios ? LaunchIOS(title_id, hang_ppc) : LaunchPPCTitle(title_id);
}

bool ESDevice::LaunchIOS(u64 ios_title_id, HangPPC hang_ppc)
{
  // BC (1-100) is only ever used on hardware to drop the clock speed and go through boot2 into
  // MIOS. With no boot1/boot2 to emulate, BC simply stands for MIOS.
  if (ios_title_id == Titles::BC)
  {
    NOTICE_LOG_FMT(IOS, "BC: Launching MIOS...");
    return LaunchIOS(Titles::MIOS, hang_ppc);
  }

  // Other IOS versions are emulated and need not be installed. MIOS does need its binary.
  if (ios_title_id != Titles::MIOS)
    return GetEmulationKernel().BootIOS(ios_title_id, hang_ppc);

  const ES::TMDReader tmd = m_core.FindInstalledTMD(ios_title_id);
  const ES::TicketReader ticket = m_core.FindSignedTicket(ios_title_id);
  ES::Content boot_content;
  const bool booted =
      tmd.IsValid() && ticket.IsValid() && tmd.GetContent(tmd.GetBootIndex(), &boot_content) &&
      GetEmulationKernel().BootIOS(ios_title_id, hang_ppc,
                                   m_core.GetContentPath(ios_title_id, boot_content));
  if (!booted)
  {
    PanicAlertFmtT("Could not launch IOS {0:016x} because it is missing from the NAND.\n"
                   "The emulated software will likely hang now.",
                   ios_title_id);
  }
  return booted;
}

s32 ESDevice::WriteLaunchFile(const ES::TMDReader& tmd, Ticks ticks)
{
  GetEmulationKernel().GetFSCore().DeleteFile(PID_KERNEL, PID_KERNEL, SPACE_FILE_PATH, ticks);

  // Title ID (host order), a ticket view (left blank: nothing reads it back), then the TMD.
  const u64 title_id = tmd.GetTitleId();
  const std::vector<u8>& tmd_bytes = tmd.GetBytes();
  std::vector<u8> contents(sizeof(u64) + sizeof(ES::TicketView) + tmd_bytes.size());
  std::memcpy(contents.data(), &title_id, sizeof(u64));
  std::ranges::copy(tmd_bytes, contents.begin() + sizeof(u64) + sizeof(ES::TicketView));
  return WriteSystemFile(LAUNCH_FILE_PATH, contents, ticks);
}

bool ESDevice::LaunchPPCTitle(u64 title_id)
{
  u64 ticks = 0;
  const ES::TMDReader tmd = m_core.FindInstalledTMD(title_id, &ticks);
  const ES::TicketReader ticket = m_core.FindSignedTicket(title_id);

  if (!tmd.IsValid() || !ticket.IsValid())
  {
    if (title_id != Titles::SYSTEM_MENU)
    {
      PanicAlertFmtT("Could not launch title {0:016x} because it is missing from the NAND.\n"
                     "The emulated software will likely hang now.",
                     title_id);
      return false;
    }
    PanicAlertFmtT("Could not launch the Wii Menu because it is missing from the NAND.\n"
                   "The emulated software will likely hang now.");
    return false;
  }

  Core::System& system = GetSystem();
  CoreTiming::CoreTimingManager& core_timing = system.GetCoreTiming();
  EmulationKernel& kernel = GetEmulationKernel();
  FSCore& fs_core = kernel.GetFSCore();

  // Launching a PPC title is a two-step affair. IOS first reloads into the IOS the TMD asks
  // for (even if it is already running), remembering the title in the launch file. The new IOS
  // finds that file at start-up and calls back into here, at which point the PPC is started.
  const bool needs_ios_reload =
      fs_core.Open(PID_KERNEL, PID_KERNEL, LAUNCH_FILE_PATH, FS::Mode::Read, {}, &ticks).Get() < 0;
  if (needs_ios_reload)
  {
    if (WriteLaunchFile(tmd, &ticks) != IPC_SUCCESS)
    {
      PanicAlertFmt("LaunchPPCTitle: Failed to write launch file");
      return false;
    }
    const u64 ios_id = tmd.GetIOSId();
    if (!Core::IsRunning(system))
      return LaunchTitle(ios_id, HangPPC::Yes);
    core_timing.RemoveEvent(s_reload_ios_for_ppc_launch_event);
    core_timing.ScheduleEvent(static_cast<s64>(ticks), s_reload_ios_for_ppc_launch_event, ios_id);
    return true;
  }

  // Second step. (The title ID stored in the launch file is not checked.)
  fs_core.DeleteFile(PID_KERNEL, PID_KERNEL, LAUNCH_FILE_PATH, &ticks);
  WriteSystemFile(SPACE_FILE_PATH, std::vector<u8>(SPACE_FILE_SIZE), &ticks);

  m_core.m_title_context.Update(tmd, ticket, DiscIO::Platform::WiiWAD);
  INFO_LOG_FMT(IOS_ES, "LaunchPPCTitle: Title context changed: {:016x}", tmd.GetTitleId());

  // IOS also does this for IOS titles, but those need not be installed here.
  ES::UIDSys uid_sys{fs_core};
  if (!UpdateUIDAndGID(kernel, &uid_sys, m_core.m_title_context.tmd))
  {
    m_core.m_title_context.Clear();
    INFO_LOG_FMT(IOS_ES, "LaunchPPCTitle: Title context changed: (none)");
    return false;
  }

  ES::Content boot_content;
  if (!tmd.GetContent(tmd.GetBootIndex(), &boot_content))
    return false;
  m_pending_ppc_boot_content_path = m_core.GetContentPath(tmd.GetTitleId(), boot_content);

  if (!Core::IsRunning(system))
    return BootstrapPPC();

  AchievementManager::GetInstance().LoadGame(nullptr);
  core_timing.RemoveEvent(s_bootstrap_ppc_for_launch_event);
  core_timing.ScheduleEvent(static_cast<s64>(ticks), s_bootstrap_ppc_for_launch_event);
  return true;
}

bool ESDevice::BootstrapPPC()
{
  const std::string path = std::exchange(m_pending_ppc_boot_content_path, {});
  return GetEmulationKernel().BootstrapPPC(path);
}

std::optional<IPCReply> ESDevice::Launch(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 0))
    return IPCReply(ES_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u64 title_id = memory.Read_U64(request.in_vectors[0].address);
  // The second vector is a ticket view; only a few of its fields are logged.
  const u32 view = request.in_vectors[1].address;
  const u32 view_version = memory.Read_U32(view);
  const u64 view_ticket_id = memory.Read_U64(view + 4);
  const u32 view_device_id = memory.Read_U32(view + 12);
  const u64 view_title_id = memory.Read_U64(view + 16);
  const u16 view_access = memory.Read_U16(view + 24);
  INFO_LOG_FMT(IOS_ES, "IOCTL_ES_LAUNCH {:016x} {:08x} {:016x} {:08x} {:016x} {:04x}", title_id,
               view_version, view_ticket_id, view_device_id, view_title_id, view_access);

  // Installed IOS versions that are not emulated cannot be launched.
  if (!IsEmulated(title_id))
    return IPCReply(FS_ENOENT);

  // A failed launch is answered normally. A successful one restarts IOS (possibly destroying
  // this device), and the new IOS acknowledges by itself, so no reply is written.
  if (!LaunchTitle(title_id))
    return IPCReply(FS_ENOENT);
  return std::nullopt;
}

std::optional<IPCReply> ESDevice::LaunchBC(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 0))
    return IPCReply(ES_EINVAL);

  // IOS refuses this in GameCube mode; running MIOS is the equivalent here.
  if (GetEmulationKernel().GetVersion() == 0x101)
    return IPCReply(ES_EINVAL);

  if (!LaunchTitle(Titles::BC))
    return IPCReply(FS_ENOENT);
  return std::nullopt;
}

// -----------------------------------------------------------------------------------------------
// DI verification

// IOS implements DIVerify as an ioctlv, but it is meant for internal use and current IOS
// versions do not let the PPC call it.
IPCReply ESDevice::DIVerify(const IOCtlVRequest& /*request*/)
{
  return IPCReply(ES_EINVAL);
}

ReturnCode ESDevice::DIVerify(const ES::TMDReader& tmd, const ES::TicketReader& ticket)
{
  m_core.m_title_context.Clear();
  INFO_LOG_FMT(IOS_ES, "ES_DIVerify: Title context changed: (none)");

  if (!tmd.IsValid() || !ticket.IsValid() || tmd.GetTitleId() != ticket.GetTitleId())
    return ES_EINVAL;

  m_core.m_title_context.Update(tmd, ticket, DiscIO::Platform::WiiDisc);
  INFO_LOG_FMT(IOS_ES, "ES_DIVerify: Title context changed: {:016x}", tmd.GetTitleId());

  // Signatures are deliberately not checked: modified discs are expected to work.

  EmulationKernel& kernel = GetEmulationKernel();
  const auto fs = kernel.GetFS();
  if (!m_core.FindInstalledTMD(tmd.GetTitleId()).IsValid())
  {
    const ReturnCode written = WriteTmdForDiVerify(fs.get(), tmd);
    if (written != IPC_SUCCESS)
    {
      ERROR_LOG_FMT(IOS_ES, "DiVerify failed to write disc TMD to NAND.");
      return written;
    }
  }

  ES::UIDSys uid_sys{kernel.GetFSCore()};
  if (!UpdateUIDAndGID(kernel, &uid_sys, m_core.m_title_context.tmd))
    return ES_SHORT_READ;

  // The data directory may exist already; only setting its owner has to succeed.
  constexpr FS::Modes data_modes{Mode::ReadWrite, Mode::None, Mode::None};
  const std::string data_dir = Common::GetTitleDataPath(tmd.GetTitleId());
  fs->CreateDirectory(PID_KERNEL, PID_KERNEL, data_dir, 0, data_modes);
  return FS::ConvertResult(
      fs->SetMetadata(0, data_dir, kernel.GetUidForPPC(), kernel.GetGidForPPC(), 0, data_modes));
}

// -----------------------------------------------------------------------------------------------
// Savestates and handles

void ESCore::Context::DoState(PointerWrap& p)
{
  p.Do(uid);
  p.Do(gid);
  title_import_export.DoState(p);
  p.Do(active);
  p.Do(ipc_fd);
}

void ESDevice::DoState(PointerWrap& p)
{
  Device::DoState(p);

  for (ESCore::OpenedContent& slot : m_core.m_content_table)
  {
    p.Do(slot.m_opened);
    p.Do(slot.m_title_id);
    p.Do(slot.m_content);
    p.Do(slot.m_fd);
    p.Do(slot.m_uid);
  }
  m_core.m_title_context.DoState(p);
  for (Context& context : m_contexts)
    context.DoState(p);
  p.Do(m_pending_ppc_boot_content_path);
}

ESDevice::ContextArray::iterator ESDevice::FindActiveContext(s32 fd)
{
  auto it = m_contexts.begin();
  while (it != m_contexts.end() && !(it->active && it->ipc_fd == fd))
    ++it;
  return it;
}

ESDevice::ContextArray::iterator ESDevice::FindInactiveContext()
{
  auto it = m_contexts.begin();
  while (it != m_contexts.end() && it->active)
    ++it;
  return it;
}

std::optional<IPCReply> ESDevice::Open(const OpenRequest& request)
{
  const auto context = FindInactiveContext();
  if (context == m_contexts.end())
    return IPCReply{ES_FD_EXHAUSTED};

  context->active = true;
  context->uid = request.uid;
  context->gid = request.gid;
  context->ipc_fd = static_cast<s32>(request.fd);
  return Device::Open(request);
}

std::optional<IPCReply> ESDevice::Close(u32 fd)
{
  const auto context = FindActiveContext(static_cast<s32>(fd));
  if (context == m_contexts.end())
    return IPCReply(ES_EINVAL);

  context->active = false;
  context->ipc_fd = -1;
  INFO_LOG_FMT(IOS_ES, "ES: Close");
  m_is_active = false;
  return IPCReply(IPC_SUCCESS);
}

std::optional<IPCReply> ESDevice::IOCtlV(const IOCtlVRequest& request)
{
  DEBUG_LOG_FMT(IOS_ES, "{} ({:#x})", GetDeviceName(), request.request);
  const auto context = FindActiveContext(static_cast<s32>(request.fd));
  if (context == m_contexts.end())
    return IPCReply(ES_EINVAL);

  Context& ctx = *context;
  const u32 uid = ctx.uid;
  // clang-format off
  switch (request.request)
  {
  // Import (per-handle state)
  case IOCTL_ES_ADDTITLESTART: return ImportTitleInit(ctx, request);
  case IOCTL_ES_ADDTMD: return ImportTmd(ctx, request);
  case IOCTL_ES_ADDCONTENTSTART: return ImportContentBegin(ctx, request);
  case IOCTL_ES_ADDCONTENTDATA: return ImportContentData(ctx, request);
  case IOCTL_ES_ADDCONTENTFINISH: return ImportContentEnd(ctx, request);
  case IOCTL_ES_ADDTITLEFINISH: return ImportTitleDone(ctx, request);
  case IOCTL_ES_ADDTITLECANCEL: return ImportTitleCancel(ctx, request);
  case IOCTL_ES_ADDTICKET: return ImportTicket(request);
  // Export (per-handle state)
  case IOCTL_ES_EXPORTTITLEINIT: return ExportTitleInit(ctx, request);
  case IOCTL_ES_EXPORTCONTENTBEGIN: return ExportContentBegin(ctx, request);
  case IOCTL_ES_EXPORTCONTENTDATA: return ExportContentData(ctx, request);
  case IOCTL_ES_EXPORTCONTENTEND: return ExportContentEnd(ctx, request);
  case IOCTL_ES_EXPORTTITLEDONE: return ExportTitleDone(ctx, request);
  // Removal
  case IOCTL_ES_DELETETITLE: return DeleteTitle(request);
  case IOCTL_ES_DELETETITLECONTENT: return DeleteTitleContent(request);
  case IOCTL_ES_DELETETICKET: return DeleteTicket(request);
  case IOCTL_ES_DELETESHAREDCONTENT: return DeleteSharedContent(request);
  case IOCTL_ES_DELETE_CONTENT: return DeleteContent(request);
  // Content access (checked against the caller's UID)
  case IOCTL_ES_OPENCONTENT: return OpenContent(uid, request);
  case IOCTL_ES_OPEN_ACTIVE_TITLE_CONTENT: return OpenActiveTitleContent(uid, request);
  case IOCTL_ES_READCONTENT: return ReadContent(uid, request);
  case IOCTL_ES_SEEKCONTENT: return SeekContent(uid, request);
  case IOCTL_ES_CLOSECONTENT: return CloseContent(uid, request);
  case IOCTL_ES_ENCRYPT: return Encrypt(uid, request);
  case IOCTL_ES_DECRYPT: return Decrypt(uid, request);
  case IOCTL_ES_SETUID: return SetUID(uid, request);
  // Listings
  case IOCTL_ES_GETTITLECNT: return GetTitleCount(request);
  case IOCTL_ES_GETTITLES: return GetTitles(request);
  case IOCTL_ES_GETOWNEDTITLECNT: return GetOwnedTitleCount(request);
  case IOCTL_ES_GETOWNEDTITLES: return GetOwnedTitles(request);
  case IOCTL_ES_GETTITLECONTENTSCNT: return GetStoredContentsCount(request);
  case IOCTL_ES_GETTITLECONTENTS: return GetStoredContents(request);
  case IOCTL_ES_GETSTOREDCONTENTCNT: return GetTMDStoredContentsCount(request);
  case IOCTL_ES_GETSTOREDCONTENTS: return GetTMDStoredContents(request);
  case IOCTL_ES_GETSHAREDCONTENTCNT: return GetSharedContentsCount(request);
  case IOCTL_ES_GETSHAREDCONTENTS: return GetSharedContents(request);
  case IOCTL_ES_GETSTOREDTMDSIZE: return GetStoredTMDSize(request);
  case IOCTL_ES_GETSTOREDTMD: return GetStoredTMD(request);
  // Views
  case IOCTL_ES_GETVIEWCNT: return GetTicketViewCount(request);
  case IOCTL_ES_GETVIEWS: return GetTicketViews(request);
  case IOCTL_ES_GETTMDVIEWCNT: return GetTMDViewSize(request);
  case IOCTL_ES_GETTMDVIEWS: return GetTMDViews(request);
  case IOCTL_ES_DIGETTICKETVIEW: return DIGetTicketView(request);
  case IOCTL_ES_DIGETTMDVIEWSIZE: return DIGetTMDViewSize(request);
  case IOCTL_ES_DIGETTMDVIEW: return DIGetTMDView(request);
  case IOCTL_ES_DIGETTMDSIZE: return DIGetTMDSize(request);
  case IOCTL_ES_DIGETTMD: return DIGetTMD(request);
  case IOCTL_ES_GET_V0_TICKET_FROM_VIEW: return GetV0TicketFromView(request);
  case IOCTL_ES_GET_TICKET_SIZE_FROM_VIEW: return GetTicketSizeFromView(request);
  case IOCTL_ES_GET_TICKET_FROM_VIEW: return GetTicketFromView(request);
  // Identity and signing
  case IOCTL_ES_GETDEVICEID: return GetDeviceId(request);
  case IOCTL_ES_GETDEVICECERT: return GetDeviceCertificate(request);
  case IOCTL_ES_SIGN: return Sign(request);
  case IOCTL_ES_VERIFYSIGN: return VerifySign(request);
  case IOCTL_ES_CHECKKOREAREGION: return CheckKoreaRegion(request);
  case IOCTL_ES_GETBOOT2VERSION: return GetBoot2Version(request);
  // Running titles
  case IOCTL_ES_GETTITLEDIR: return GetTitleDirectory(request);
  case IOCTL_ES_GETTITLEID: return GetTitleId(request);
  case IOCTL_ES_GETCONSUMPTION: return GetConsumption(request);
  case IOCTL_ES_LAUNCH: return Launch(request);
  case IOCTL_ES_LAUNCHBC: return LaunchBC(request);
  case IOCTL_ES_DIVERIFY:
  case IOCTL_ES_DIVERIFY_WITH_VIEW: return DIVerify(request);
  case IOCTL_ES_SET_UP_STREAM_KEY: return SetUpStreamKey(ctx, request);
  case IOCTL_ES_DELETE_STREAM_KEY: return DeleteStreamKey(request);

  case IOCTL_ES_UNKNOWN_41:
  case IOCTL_ES_UNKNOWN_42:
    PanicAlertFmt("IOS-ES: Unimplemented ioctlv {:#x} ({} in vectors, {} io vectors)",
                  request.request, request.in_vectors.size(), request.io_vectors.size());
    request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_ES,
                        Common::Log::LogLevel::LERROR);
    return IPCReply(IPC_EINVAL);

  default:  // including IOCTL_ES_INVALID_3F and IOCTL_ES_IMPORTBOOT
    return IPCReply(IPC_EINVAL);
  }
  // clang-format on
}

// -----------------------------------------------------------------------------------------------
// Stream keys (used by WFS titles)

ReturnCode ESCore::CheckStreamKeyPermissions(const u32 uid, const u8* ticket_view,
                                             const ES::TMDReader& tmd) const
{
  // IOS requires exactly one of these two title flags to be set.
  const u32 flags = tmd.GetTitleFlags();
  const bool has_0x4 = (flags & ES::TITLE_TYPE_0x4) != 0;
  const bool has_wfs = (flags & ES::TITLE_TYPE_WFS_MAYBE) != 0;
  if (has_0x4 == has_wfs)
    return ES_EINVAL;

  // Only PPC titles and the WFS modules (PID_UNKNOWN, 0x19) may ask; WFS-flagged titles are
  // reserved for the WFS modules.
  const bool from_wfs = uid == PID_UNKNOWN;
  if (!from_wfs && (uid < FIRST_PPC_UID || has_wfs))
    return ES_EINVAL;

  if (Common::swap64(ticket_view + offsetof(ES::TicketView, title_id)) != tmd.GetTitleId())
    return ES_EINVAL;

  const u32 mask = Common::swap32(ticket_view + offsetof(ES::TicketView, permitted_title_mask));
  const u32 permitted = Common::swap32(ticket_view + offsetof(ES::TicketView, permitted_title_id));
  if (from_wfs && (~mask & 0x13) != permitted)
    return ES_EACCES;
  if (!IsActiveTitlePermittedByTicket(ticket_view))
    return ES_EACCES;
  return IPC_SUCCESS;
}

ReturnCode ESCore::SetUpStreamKey(const u32 uid, const u8* ticket_view, const ES::TMDReader& tmd,
                                  u32* handle)
{
  ReturnCode ret = CheckStreamKeyPermissions(uid, ticket_view, tmd);
  if (ret != IPC_SUCCESS)
    return ret;

  // The installed ticket that this view was made from. A missing ticket file is reported as a
  // file system error here, unlike in the other view lookups.
  const u64 title_id = Common::swap64(ticket_view + offsetof(ES::TicketView, title_id));
  const u64 ticket_id = Common::swap64(ticket_view + offsetof(ES::TicketView, ticket_id));
  const ES::TicketReader installed = FindSignedTicket(title_id);
  if (!installed.IsValid())
    return FS_ENOENT;
  const std::vector<u8> ticket = installed.GetRawTicket(ticket_id);
  if (ticket.empty())
    return ES_NO_TICKET;

  std::vector<u8> cert_store;
  ret = ReadCertStore(&cert_store);
  if (ret != IPC_SUCCESS)
    return ret;
  ret = VerifyContainer(VerifyContainerType::TMD, VerifyMode::UpdateCertStore, tmd, cert_store);
  if (ret != IPC_SUCCESS)
    return ret;
  ret = VerifyContainer(VerifyContainerType::Ticket, VerifyMode::UpdateCertStore, installed,
                        cert_store);
  if (ret != IPC_SUCCESS)
    return ret;

  IOSC& iosc = m_ios.GetIOSC();
  ret = iosc.CreateObject(handle, IOSC::TYPE_SECRET_KEY, IOSC::ObjectSubType::AES128, PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;

  // PPC callers share one owner bit.
  const u32 owner = uid >= FIRST_PPC_UID ? PID_PPCBOOT : uid;
  ret = iosc.SetOwnership(*handle, 1u << owner, PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;

  const u8 key_index = ticket[offsetof(ES::Ticket, common_key_index)];
  if (key_index >= IOSC::COMMON_KEY_HANDLES.size())
    return ES_INVALID_TICKET;

  // Title key IV: the title ID in host byte order, zero padded.
  std::array<u8, 16> iv{};
  std::memcpy(iv.data(), &title_id, sizeof(u64));
  return iosc.ImportSecretKey(*handle, IOSC::COMMON_KEY_HANDLES[key_index], iv.data(),
                              ticket.data() + offsetof(ES::Ticket, title_key), PID_ES);
}

IPCReply ESDevice::SetUpStreamKey(const Context& context, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 1))
    return IPCReply(ES_EINVAL);
  const IOCtlVRequest::IOVector& view_vec = request.in_vectors[0];
  const IOCtlVRequest::IOVector& tmd_vec = request.in_vectors[1];
  const IOCtlVRequest::IOVector& out_vec = request.io_vectors[0];
  if (view_vec.size != sizeof(ES::TicketView) || !ES::IsValidTMDSize(tmd_vec.size) ||
      out_vec.size != sizeof(u32))
  {
    return IPCReply(ES_EINVAL);
  }

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  std::vector<u8> tmd_bytes(tmd_vec.size);
  memory.CopyFromEmu(tmd_bytes.data(), tmd_vec.address, tmd_bytes.size());
  const ES::TMDReader tmd{std::move(tmd_bytes)};
  if (!tmd.IsValid())
    return IPCReply(ES_EINVAL);

  // The handle is written back even on failure. It starts at zero so that a failed call writes
  // a defined value.
  u32 handle = 0;
  const u8* view = memory.GetPointerForRange(view_vec.address, sizeof(ES::TicketView));
  const ReturnCode ret = m_core.SetUpStreamKey(context.uid, view, tmd, &handle);
  memory.Write_U32(handle, out_vec.address);
  return IPCReply(ret);
}

IPCReply ESDevice::DeleteStreamKey(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0) || request.in_vectors[0].size != sizeof(u32))
    return IPCReply(ES_EINVAL);

  const u32 handle = GetSystem().GetMemory().Read_U32(request.in_vectors[0].address);
  return IPCReply(GetEmulationKernel().GetIOSC().DeleteObject(handle, PID_ES));
}

bool ESCore::IsActiveTitlePermittedByTicket(const u8* ticket_view) const
{
  if (!m_title_context.active)
    return false;

  const auto low_title_id = static_cast<u32>(m_title_context.tmd.GetTitleId());
  const u32 mask = Common::swap32(ticket_view + offsetof(ES::TicketView, permitted_title_mask));
  const u32 permitted = Common::swap32(ticket_view + offsetof(ES::TicketView, permitted_title_id));
  return low_title_id != 0 && (low_title_id & ~mask) == permitted;
}

// -----------------------------------------------------------------------------------------------
// Certificates and signatures

bool ESCore::IsIssuerCorrect(VerifyContainerType type, const ES::CertReader& issuer_cert) const
{
  // TMDs are signed by CP certificates, tickets by XS, console certificates by MS.
  std::string_view prefix;
  switch (type)
  {
  case VerifyContainerType::TMD:
    prefix = "CP";
    break;
  case VerifyContainerType::Ticket:
    prefix = "XS";
    break;
  case VerifyContainerType::Device:
    prefix = "MS";
    break;
  default:
    return false;
  }
  return issuer_cert.GetName().starts_with(prefix);
}

ReturnCode ESCore::ReadCertStore(std::vector<u8>* buffer) const
{
  const auto file = m_ios.GetFS()->OpenFile(PID_KERNEL, PID_KERNEL, CERT_STORE_PATH, Mode::Read);
  if (!file)
    return FS::ConvertResult(file.error());

  buffer->resize(file->GetStatus()->size);
  return file->Read(buffer->data(), buffer->size()) ? IPC_SUCCESS : ES_SHORT_READ;
}

ReturnCode ESCore::WriteNewCertToStore(const ES::CertReader& cert)
{
  // Nothing to do if a certificate with this name is stored already.
  std::vector<u8> store;
  if (ReadCertStore(&store) == IPC_SUCCESS && ES::ParseCertChain(store).contains(cert.GetName()))
    return IPC_SUCCESS;

  const auto file =
      m_ios.GetFS()->CreateAndOpenFile(PID_KERNEL, PID_KERNEL, CERT_STORE_PATH, WORLD_READABLE);
  if (!file || !file->Seek(0, FS::SeekMode::End))
    return ES_EIO;
  const std::vector<u8>& bytes = cert.GetBytes();
  return file->Write(bytes.data(), bytes.size()) ? IPC_SUCCESS : ES_EIO;
}

ReturnCode ESCore::VerifyContainer(VerifyContainerType type, VerifyMode mode,
                                   const ES::SignedBlobReader& signed_blob,
                                   const std::vector<u8>& cert_chain, u32* issuer_handle_out)
{
  if (!signed_blob.IsSignatureValid())
    return ES_EINVAL;

  // The issuer string names exactly three levels, e.g. "Root-CA00000001-XS00000003". IOS takes
  // it for granted that the root signed the CA and the CA signed the issuer.
  const std::vector<std::string> chain_names = SplitString(signed_blob.GetIssuer(), '-');
  if (chain_names.size() != 3)
    return ES_EINVAL;

  const std::map<std::string, ES::CertReader> certs = ES::ParseCertChain(cert_chain);
  const auto issuer_it = certs.find(chain_names[2]);
  const auto ca_it = certs.find(chain_names[1]);
  if (issuer_it == certs.end() || ca_it == certs.end())
    return ES_UNKNOWN_ISSUER;
  const ES::CertReader& issuer_cert = issuer_it->second;
  const ES::CertReader& ca_cert = ca_it->second;

  if (!IsIssuerCorrect(type, issuer_cert))
    return ES_EINVAL;

  IOSC& iosc = m_ios.GetIOSC();

  // CA key, checked against the root key.
  IOSC::Handle ca_handle;
  ReturnCode ret =
      iosc.CreateObject(&ca_handle, IOSC::TYPE_PUBLIC_KEY, IOSC::ObjectSubType::RSA2048, PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;
  Common::ScopeGuard free_ca{[&] { iosc.DeleteObject(ca_handle, PID_ES); }};
  ret = iosc.ImportCertificate(ca_cert, IOSC::HANDLE_ROOT_KEY, ca_handle, PID_ES);
  if (ret != IPC_SUCCESS)
  {
    ERROR_LOG_FMT(IOS_ES, "VerifyContainer: IOSC_ImportCertificate(ca) failed with error {}",
                  std::to_underlying(ret));
    return ret;
  }

  // Issuer key, checked against the CA key. Console certificates are signed with ECC keys.
  const IOSC::ObjectSubType issuer_key_type = type == VerifyContainerType::Device ?
                                                  IOSC::ObjectSubType::ECC233 :
                                                  IOSC::ObjectSubType::RSA2048;
  IOSC::Handle issuer_handle;
  ret = iosc.CreateObject(&issuer_handle, IOSC::TYPE_PUBLIC_KEY, issuer_key_type, PID_ES);
  if (ret != IPC_SUCCESS)
    return ret;
  Common::ScopeGuard free_issuer{[&] { iosc.DeleteObject(issuer_handle, PID_ES); }};
  ret = iosc.ImportCertificate(issuer_cert, ca_handle, issuer_handle, PID_ES);
  if (ret != IPC_SUCCESS)
  {
    ERROR_LOG_FMT(IOS_ES, "VerifyContainer: IOSC_ImportCertificate(issuer) failed with error {}",
                  std::to_underlying(ret));
    return ret;
  }

  // Finally the blob itself.
  ret = iosc.VerifyPublicKeySign(signed_blob.GetSha1(), issuer_handle,
                                 signed_blob.GetSignatureData(), PID_ES);
  if (ret != IPC_SUCCESS)
  {
    ERROR_LOG_FMT(IOS_ES, "VerifyContainer: IOSC_VerifyPublicKeySign failed with error {}",
                  std::to_underlying(ret));
    return ret;
  }

  if (mode == VerifyMode::UpdateCertStore)
  {
    // Only the result of storing the CA certificate decides the return value.
    const ReturnCode issuer_written = WriteNewCertToStore(issuer_cert);
    if (issuer_written != IPC_SUCCESS)
    {
      ERROR_LOG_FMT(IOS_ES, "VerifyContainer: Writing the issuer cert failed with return code {}",
                    std::to_underlying(issuer_written));
    }
    ret = WriteNewCertToStore(ca_cert);
    if (ret != IPC_SUCCESS)
    {
      ERROR_LOG_FMT(IOS_ES, "VerifyContainer: Writing the CA cert failed with return code {}",
                    std::to_underlying(ret));
    }
  }

  if (ret == IPC_SUCCESS && issuer_handle_out != nullptr)
  {
    // Hand the issuer key over to the caller instead of freeing it.
    *issuer_handle_out = issuer_handle;
    free_issuer.Dismiss();
  }
  return ret;
}

ReturnCode ESCore::VerifyContainer(VerifyContainerType type, VerifyMode mode,
                                   const ES::CertReader& cert, const std::vector<u8>& cert_chain,
                                   u32 certificate_iosc_handle)
{
  IOSC::Handle issuer_handle;
  ReturnCode ret = VerifyContainer(type, mode, cert, cert_chain, &issuer_handle);
  if (ret != IPC_SUCCESS)
    return ret;

  // Store the verified certificate's key, then drop the issuer key.
  IOSC& iosc = m_ios.GetIOSC();
  ret = iosc.ImportCertificate(cert, issuer_handle, certificate_iosc_handle, PID_ES);
  iosc.DeleteObject(issuer_handle, PID_ES);
  return ret;
}
}  // namespace IOS::HLE
