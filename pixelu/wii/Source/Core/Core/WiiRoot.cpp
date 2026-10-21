// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/WiiRoot.h"

#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Core/Boot/Boot.h"
#include "Core/CommonTitles.h"
#include "Core/ConfigManager.h"
#include "Core/HW/WiiSave.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/Uids.h"
#include "Core/Movie.h"
#include "Core/NetPlayClient.h"
#include "Core/SysConf.h"
#include "Core/System.h"

namespace Core
{
namespace FS = IOS::HLE::FS;

namespace
{
// Everyone may read and write.
constexpr FS::Modes OPEN_MODES{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite};

// NAND paths of the files BackupWiiSettings protects.
std::string SettingTxtPath()
{
  return Common::GetTitleDataPath(Titles::SYSTEM_MENU) + "/" WII_SETTING;
}
constexpr const char* SYSCONF_NAND_PATH = "/shared2/sys/SYSCONF";

struct RootState
{
  // Host folders of the temporary NAND and of redirected saves; both empty
  // unless the root is temporary.
  std::string temp_root;
  std::string temp_redirect_root;
  bool initialized = false;
  std::vector<FS::NandRedirect> redirects;

  // With a temporary NAND, a save redirect points into temp_redirect_root
  // instead of its real folder; this remembers where to move it back to.
  struct DeferredRedirect
  {
    std::string real_path;
    std::string temp_path;
  };
  std::vector<DeferredRedirect> deferred_redirects;
};
RootState s_root;

// Backups live in the user's Backup folder under the file's own name.
std::string BackupPathFor(const std::string& file_name)
{
  return File::GetUserPath(D_BACKUP_IDX) + file_name;
}

bool CopyIfPresent(const std::string& from, const std::string& to)
{
  if (!File::Exists(from))
    return false;
  File::CreateFullPath(to);
  return File::CopyRegularFile(from, to);
}

void BackUpNandFile(const std::string& nand_path)
{
  CopyIfPresent(File::GetUserPath(D_WIIROOT_IDX) + nand_path,
                BackupPathFor(PathToFileName(nand_path)));
}

void RestoreNandFile(const std::string& nand_path)
{
  const std::string name = PathToFileName(nand_path);
  if (CopyIfPresent(BackupPathFor(name), File::GetUserPath(D_WIIROOT_IDX) + nand_path))
    File::Delete(BackupPathFor(name));
}

void CopyTitleSave(FS::FileSystem* from, FS::FileSystem* to, u64 title_id)
{
  to->CreateFullPath(IOS::PID_KERNEL, IOS::PID_KERNEL, Common::GetTitleDataPath(title_id) + '/', 0,
                     OPEN_MODES);
  const auto from_storage = WiiSave::MakeNandStorage(from, title_id);
  const auto to_storage = WiiSave::MakeNandStorage(to, title_id);
  WiiSave::Copy(from_storage.get(), to_storage.get());
}

// Copies one NAND file between file systems. A missing source is not an
// error and must not leave an empty file behind.
bool CopyFileBetweenNands(FS::FileSystem* from, const std::string& from_path, FS::FileSystem* to,
                          const std::string& to_path)
{
  const auto input = from->OpenFile(IOS::PID_KERNEL, IOS::PID_KERNEL, from_path, FS::Mode::Read);
  if (!input)
    return true;

  to->CreateFullPath(IOS::PID_KERNEL, IOS::PID_KERNEL, to_path, 0, OPEN_MODES);
  // The file is created under the source's path (both callers pass the same path twice).
  const auto output = to->CreateAndOpenFile(IOS::PID_KERNEL, IOS::PID_KERNEL, from_path, OPEN_MODES);
  if (!output)
    return false;

  std::vector<u8> contents(input->GetStatus()->size);
  return input->Read(contents.data(), contents.size()) &&
         output->Write(contents.data(), contents.size());
}

// For movies and NetPlay the temporary NAND starts with known saves: the
// recorded game's (movie), the host's synced titles (NetPlay), or none.
void SeedTemporarySaves(FS::FileSystem* session_fs, const BootSessionData& boot)
{
  auto& movie = Core::System::GetInstance().GetMovie();
  const SConfig& config = SConfig::GetInstance();
  const u64 title_id = config.GetTitleID();
  const auto user_fs = FS::MakeFileSystem(FS::Location::Configured);

  if (movie.IsRecordingInput())
  {
    if (NetPlay::IsNetPlayRunning() && !config.bCopyWiiSaveNetplay)
    {
      movie.SetClearSave(true);
    }
    else
    {
      // A banner is taken as the sign that a save exists.
      const std::string banner = Common::GetTitleDataPath(title_id) + "/banner.bin";
      movie.SetClearSave(!user_fs->GetMetadata(IOS::PID_KERNEL, IOS::PID_KERNEL, banner));
    }
  }

  const bool netplay_copies = NetPlay::IsNetPlayRunning() && config.bCopyWiiSaveNetplay;
  const bool movie_needs_save = movie.IsMovieActive() && !movie.IsStartingFromClearSave();
  if (!netplay_copies && !movie_needs_save)
    return;

  auto* const sync_fs = boot.GetWiiSyncFS();
  auto* const from = sync_fs ? sync_fs : user_fs.get();
  INFO_LOG_FMT(CORE, "Wii Save Init: Copying from {} to session_fs.",
               sync_fs ? "sync_fs" : "configured_fs");

  if (movie.IsMovieActive() && !NetPlay::IsNetPlayRunning())
  {
    INFO_LOG_FMT(CORE, "Wii Save Init: Copying {0:016x}.", title_id);
    CopyTitleSave(from, session_fs, title_id);
  }
  else
  {
    for (const u64 synced : boot.GetWiiSyncTitles())
    {
      INFO_LOG_FMT(CORE, "Wii Save Init: Copying {0:016x}.", synced);
      CopyTitleSave(from, session_fs, synced);
    }
  }

  const std::string mii_db = Common::GetMiiDatabasePath();
  if (!CopyFileBetweenNands(from, mii_db, session_fs, mii_db))
    WARN_LOG_FMT(CORE, "Failed to copy Mii database to the NAND");

  const auto& redirect_source = boot.GetWiiSyncRedirectFolder();
  if (!redirect_source.empty())
  {
    File::CreateDirs(s_root.temp_redirect_root);
    File::Copy(redirect_source, s_root.temp_redirect_root);
  }
}

// A leftover temporary folder (for example after a crash) is kept as
// "<name>.backup", replacing any older backup. `dir` ends in a separator.
void SetAsideLeftover(const std::string& dir)
{
  if (!File::Exists(dir))
    return;

  const std::string backup = dir.substr(0, dir.size() - 1) + ".backup";
  WARN_LOG_FMT(IOS_FS, "Temporary directory at {} exists, moving to backup...", dir);
  if (File::Exists(backup))
  {
    WARN_LOG_FMT(IOS_FS, "Temporary backup directory at {} exists, deleting...", backup);
    File::DeleteDirRecursively(backup);
  }
  File::MoveWithOverwrite(dir, backup);
}

// Copies the host folder `host_dir` into NAND folder `nand_dir` ("" for the
// root; neither has a trailing slash), recursing into subfolders. Files that
// already exist on the NAND are left alone.
bool InstallSystemMenuFiles(FS::FileSystem* fs, const std::string& host_dir,
                            const std::string& nand_dir)
{
  const File::FSTEntry listing = File::ScanDirectoryTree(host_dir, false);
  for (const File::FSTEntry& child : listing.children)
  {
    const std::string host_path = host_dir + '/' + child.virtualName;
    const std::string nand_path = nand_dir + '/' + child.virtualName;

    if (child.isDirectory)
    {
      fs->CreateDirectory(IOS::SYSMENU_UID, IOS::SYSMENU_GID, nand_path, 0, OPEN_MODES);
      if (!InstallSystemMenuFiles(fs, host_path, nand_path))
        return false;
      continue;
    }

    // (The existence check passes the UID as the GID too.)
    if (fs->GetMetadata(IOS::SYSMENU_UID, IOS::SYSMENU_UID, nand_path).has_value())
      continue;

    File::IOFile source{host_path, "rb"};
    std::vector<u8> contents(source.GetSize());
    if (!source.ReadBytes(contents.data(), contents.size()))
      return false;

    const auto target =
        fs->CreateAndOpenFile(IOS::SYSMENU_UID, IOS::SYSMENU_GID, nand_path, OPEN_MODES);
    if (!target || !target->Write(contents.data(), contents.size()))
      return false;
  }
  return true;
}
}  // namespace

const std::vector<IOS::HLE::FS::NandRedirect>& GetActiveNandRedirects()
{
  return s_root.redirects;
}

void InitializeWiiRoot(bool use_temporary)
{
  ASSERT(!s_root.initialized);

  if (!use_temporary)
  {
    File::SetUserPath(D_SESSION_WIIROOT_IDX, File::GetUserPath(D_WIIROOT_IDX));
  }
  else
  {
    const std::string user_dir = File::GetUserPath(D_USER_IDX);
    s_root.temp_root = user_dir + "WiiSession" DIR_SEP;
    s_root.temp_redirect_root = user_dir + "RedirectSession" DIR_SEP;
    WARN_LOG_FMT(IOS_FS, "Using temporary directory {} for minimal Wii FS", s_root.temp_root);
    WARN_LOG_FMT(IOS_FS, "Using temporary directory {} for redirected saves",
                 s_root.temp_redirect_root);

    SetAsideLeftover(s_root.temp_root);
    SetAsideLeftover(s_root.temp_redirect_root);

    File::SetUserPath(D_SESSION_WIIROOT_IDX, s_root.temp_root);
  }

  s_root.redirects.clear();
  s_root.initialized = true;
}

void ShutdownWiiRoot()
{
  if (WiiRootIsTemporary())
  {
    File::DeleteDirRecursively(s_root.temp_root);
    s_root.temp_root.clear();
    File::DeleteDirRecursively(s_root.temp_redirect_root);
    s_root.temp_redirect_root.clear();
    s_root.deferred_redirects.clear();
  }

  s_root.redirects.clear();
  s_root.initialized = false;
}

bool WiiRootIsInitialized()
{
  return s_root.initialized;
}

bool WiiRootIsTemporary()
{
  return !s_root.temp_root.empty();
}

void BackupWiiSettings()
{
  // Boot can rewrite these. setting.txt has no other mechanism that restores
  // it, so it always needs the copy. SYSCONF is normally restored by the
  // config system when emulation ends; its copy only matters after a crash.
  BackUpNandFile(SettingTxtPath());
  BackUpNandFile(SYSCONF_NAND_PATH);
}

void RestoreWiiSettings(RestoreReason reason)
{
  RestoreNandFile(SettingTxtPath());

  // After a clean exit the user may have changed SYSCONF on purpose (e.g. in
  // the Wii Menu), so only a crash brings the old copy back.
  if (reason != RestoreReason::CrashRecovery)
    File::Delete(BackupPathFor("SYSCONF"));
  else
    RestoreNandFile(SYSCONF_NAND_PATH);
}

void InitializeWiiFileSystemContents(
    std::optional<DiscIO::Riivolution::SavegameRedirect> save_redirect,
    const BootSessionData& boot_session_data)
{
  const auto fs = Core::System::GetInstance().GetIOS()->GetFS();

  // The System Menu normally creates the WiiConnect24 files, and some games
  // (Mario Kart Wii, for one) expect them from the first boot. Since we boot
  // games directly, install them from Sys.
  if (!InstallSystemMenuFiles(fs.get(), File::GetSysDirectory() + WII_USER_DIR, ""))
    WARN_LOG_FMT(CORE, "Failed to copy initial System Menu files to the NAND");

  const bool temporary = WiiRootIsTemporary();
  if (temporary)
  {
    // A fresh temporary NAND gets a default SYSCONF. The object lives until
    // the saves are seeded; its destructor saves it once more.
    SysConf default_sysconf{fs};
    default_sysconf.Save();
    SeedTemporarySaves(fs.get(), boot_session_data);
  }

  if (!save_redirect)
    return;

  const u64 title_id = SConfig::GetInstance().GetTitleID();
  std::string nand_save_dir = Common::GetTitleDataPath(title_id);
  std::string& target = save_redirect->m_target_path;

  if (temporary)
  {
    // Redirect into the temporary area; the real folder is updated at cleanup.
    s_root.deferred_redirects.push_back({target, s_root.temp_redirect_root});
    target = s_root.temp_redirect_root;
  }

  if (!File::IsDirectory(target))
  {
    File::CreateDirs(target);
    if (save_redirect->m_clone)
    {
      File::Copy(Common::GetTitleDataPath(title_id, Common::FromWhichRoot::Session), target);
    }
  }

  s_root.redirects.push_back(FS::NandRedirect{std::move(nand_save_dir), std::move(target)});
  fs->SetNandRedirects(s_root.redirects);
}

void CleanUpWiiFileSystemContents(const BootSessionData& boot_session_data)
{
  // A temporary NAND is copied back for movies always, and for NetPlay only
  // on the host with save writing enabled.
  const bool temporary = WiiRootIsTemporary();
  const auto* const netplay = boot_session_data.GetNetplaySettings();
  const bool netplay_writes = netplay && netplay->savedata_write;
  const bool netplay_hosts = netplay && netplay->is_hosting;
  const bool copy_back = temporary && (!netplay || (netplay_writes && netplay_hosts));

  INFO_LOG_FMT(CORE,
               "Wii FS Cleanup: cleanup_required = {} (wii_root_is_temporary = {}, is netplay "
               "= {}, is_netplay_write = {}, is_netplay_host = {})",
               copy_back, temporary, netplay != nullptr, netplay_writes, netplay_hosts);
  if (!copy_back)
    return;

  INFO_LOG_FMT(CORE, "Wii FS Cleanup: Copying from temporary FS to configured_fs.");

  for (const auto& deferred : s_root.deferred_redirects)
  {
    File::CreateFullPath(deferred.real_path);
    File::MoveWithOverwrite(deferred.temp_path, deferred.real_path);
  }

  IOS::HLE::EmulationKernel* const ios = Core::System::GetInstance().GetIOS();

  // Drop the redirects first, so the copy below reads the session NAND itself.
  s_root.redirects.clear();
  ios->GetFS()->SetNandRedirects({});

  const auto user_fs = FS::MakeFileSystem(FS::Location::Configured);

  const std::string mii_db = Common::GetMiiDatabasePath();
  if (!CopyFileBetweenNands(ios->GetFS().get(), mii_db, user_fs.get(), mii_db))
    WARN_LOG_FMT(CORE, "Failed to copy Mii database to the NAND");

  // When only some saves were synced in, only those go back; otherwise a save
  // created for a different game during NetPlay could replace a local one.
  const bool all_titles = !netplay || netplay->savedata_sync_all_wii;
  const std::vector<u64> titles =
      all_titles ? ios->GetESCore().GetInstalledTitles() : boot_session_data.GetWiiSyncTitles();
  for (const u64 title_id : titles)
  {
    INFO_LOG_FMT(CORE, "Wii FS Cleanup: Copying {0:016x}.", title_id);

    const auto session_save = WiiSave::MakeNandStorage(ios->GetFS().get(), title_id);

    // The FS only writes a save into an existing directory.
    user_fs->CreateFullPath(IOS::PID_KERNEL, IOS::PID_KERNEL,
                            Common::GetTitleDataPath(title_id) + '/', 0, OPEN_MODES);
    const auto user_save = WiiSave::MakeNandStorage(user_fs.get(), title_id);

    // Keep the user's previous save as Backup/<title>.bin before replacing it.
    const std::string backup_file =
        fmt::format("{}/{:016x}.bin", File::GetUserPath(D_BACKUP_IDX), title_id);
    const auto backup_save = WiiSave::MakeDataBinStorage(&ios->GetIOSC(), backup_file, "w+b");
    WiiSave::Copy(user_save.get(), backup_save.get());
    WiiSave::Copy(session_save.get(), user_save.get());
  }
}
}  // namespace Core
