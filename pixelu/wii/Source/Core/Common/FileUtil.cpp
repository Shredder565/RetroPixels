// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/FileUtil.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "Common/CommonFuncs.h"
#include "Common/CommonPaths.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <objbase.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <climits>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace File
{
// ---- Queries ---------------------------------------------------------------------------------

FileInfo::FileInfo(const std::string& path) : FileInfo(path.c_str())
{
}

FileInfo::FileInfo(const char* path)
{
  const fs::path native = StringToPath(path);
  std::error_code ec;
  m_status = fs::status(native, ec);
  m_exists = fs::exists(m_status);
  m_size = fs::file_size(native, ec);
  if (ec)
    m_size = 0;
}

bool FileInfo::Exists() const
{
  return m_exists;
}

bool FileInfo::IsDirectory() const
{
  return fs::is_directory(m_status);
}

bool FileInfo::IsFile() const
{
  return m_exists && !fs::is_directory(m_status);
}

u64 FileInfo::GetSize() const
{
  return IsFile() ? m_size : 0;
}

bool Exists(const std::string& path)
{
  return FileInfo(path).Exists();
}

bool IsDirectory(const std::string& path)
{
  return FileInfo(path).IsDirectory();
}

bool IsFile(const std::string& path)
{
  return FileInfo(path).IsFile();
}

u64 GetSize(const std::string& path)
{
  return FileInfo(path).GetSize();
}

u64 GetSize(FILE* f)
{
  const auto position = ftello(f);
  if (fseeko(f, 0, SEEK_END) != 0)
  {
    ERROR_LOG_FMT(COMMON, "GetSize: seek failed {}: {}", fmt::ptr(f), Common::LastStrerrorString());
    return 0;
  }
  const auto end = ftello(f);
  if (end != position && fseeko(f, position, SEEK_SET) != 0)
  {
    ERROR_LOG_FMT(COMMON, "GetSize: seek failed {}: {}", fmt::ptr(f), Common::LastStrerrorString());
    return 0;
  }
  return static_cast<u64>(end);
}

// ---- Creating and deleting -------------------------------------------------------------------

namespace
{
// create_directory/create_directories report "nothing created" for an existing directory, which
// callers treat as success.
template <typename CreateFn>
bool EnsureDirectory(const fs::path& path, CreateFn create, std::string_view what,
                     std::string_view shown_path)
{
  std::error_code ec;
  if (create(path, ec))
    return true;
  std::error_code ignored;
  if (fs::is_directory(path, ignored))
    return true;
  ERROR_LOG_FMT(COMMON, "{}: failed on {}: {}", what, shown_path, ec.message());
  return false;
}

bool CreateOne(const fs::path& path, std::error_code& ec)
{
  return fs::create_directory(path, ec);
}

bool CreateAll(const fs::path& path, std::error_code& ec)
{
  return fs::create_directories(path, ec);
}
}  // namespace

bool CreateDir(const std::string& path)
{
  DEBUG_LOG_FMT(COMMON, "CreateDir: directory {}", path);
  return EnsureDirectory(StringToPath(path), CreateOne, "CreateDir", path);
}

bool CreateDirs(std::string_view path)
{
  DEBUG_LOG_FMT(COMMON, "CreateDirs: directory {}", path);
  return EnsureDirectory(StringToPath(path), CreateAll, "CreateDirs", path);
}

bool CreateFullPath(std::string_view fullPath)
{
  DEBUG_LOG_FMT(COMMON, "CreateFullPath: path {}", fullPath);
  return EnsureDirectory(StringToPath(fullPath).parent_path(), CreateAll, "CreateFullPath",
                         fullPath);
}

bool Delete(const std::string& filename, IfAbsentBehavior behavior)
{
  DEBUG_LOG_FMT(COMMON, "Delete: file {}", filename);

  const fs::path native = StringToPath(filename);
  std::error_code ec;
  const fs::file_status status = fs::status(native, ec);

  if (!fs::exists(status))
  {
    // The caller wants the file gone, and it is.
    if (behavior == IfAbsentBehavior::ConsoleWarning)
      WARN_LOG_FMT(COMMON, "Delete: {} does not exist", filename);
    return true;
  }
  if (fs::is_directory(status))
  {
    WARN_LOG_FMT(COMMON, "Delete failed: {} is a directory", filename);
    return false;
  }
  if (!fs::remove(native, ec))
  {
    WARN_LOG_FMT(COMMON, "Delete: failed on {}: {}", filename, ec.message());
    return false;
  }
  return true;
}

bool DeleteDir(const std::string& filename, IfAbsentBehavior behavior)
{
  DEBUG_LOG_FMT(COMMON, "DeleteDir: directory {}", filename);

  const fs::path native = StringToPath(filename);
  std::error_code ec;
  const fs::file_status status = fs::status(native, ec);

  if (!fs::exists(status))
  {
    if (behavior == IfAbsentBehavior::ConsoleWarning)
      WARN_LOG_FMT(COMMON, "DeleteDir: {} does not exist", filename);
    return true;
  }
  if (!fs::is_directory(status))
  {
    ERROR_LOG_FMT(COMMON, "DeleteDir: Not a directory {}", filename);
    return false;
  }
  if (!fs::remove(native, ec))
  {
    WARN_LOG_FMT(COMMON, "DeleteDir: failed on {}: {}", filename, ec.message());
    return false;
  }
  return true;
}

bool DeleteDirRecursively(const std::string& directory)
{
  DEBUG_LOG_FMT(COMMON, "DeleteDirRecursively: {}", directory);

  std::error_code ec;
  const std::uintmax_t removed = fs::remove_all(StringToPath(directory), ec);
  const bool ok = !ec && removed != static_cast<std::uintmax_t>(-1);
  if (!ok)
    ERROR_LOG_FMT(COMMON, "DeleteDirRecursively: {} failed. {}", directory, ec.message());
  return ok;
}

bool CreateEmptyFile(const std::string& filename)
{
  DEBUG_LOG_FMT(COMMON, "CreateEmptyFile: {}", filename);
  if (IOFile(filename, "wb"))
    return true;
  ERROR_LOG_FMT(COMMON, "CreateEmptyFile: failed {}: {}", filename, Common::LastStrerrorString());
  return false;
}

// ---- Renaming, copying, moving ---------------------------------------------------------------

bool Rename(const std::string& srcFilename, const std::string& destFilename)
{
  DEBUG_LOG_FMT(COMMON, "Rename: {} --> {}", srcFilename, destFilename);
  std::error_code ec;
  fs::rename(StringToPath(srcFilename), StringToPath(destFilename), ec);
  if (ec)
  {
    ERROR_LOG_FMT(COMMON, "Rename failed: {} --> {}: {}", srcFilename, destFilename,
                  ec.message());
    return false;
  }
  return true;
}

namespace
{
#ifndef _WIN32
void SyncToDisk(const fs::path& path)
{
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return;
  fsync(fd);
  close(fd);
}
#endif
}  // namespace

bool RenameSync(const std::string& srcFilename, const std::string& destFilename)
{
  if (!Rename(srcFilename, destFilename))
    return false;

#ifdef _WIN32
  // Flush the file's data (opened under its old name, as before the rename).
  int fd = -1;
  const errno_t err = _wsopen_s(&fd, UTF8ToWString(srcFilename).c_str(), _O_RDONLY, _SH_DENYNO,
                                _S_IREAD | _S_IWRITE);
  if (err == 0 && fd >= 0)
  {
    if (_commit(fd) != 0)
      ERROR_LOG_FMT(COMMON, "RenameSync sync failed on {}: {}", srcFilename, err);
    _close(fd);
  }
#else
  // Make the file contents and both directory entries durable.
  const fs::path source = StringToPath(srcFilename);
  SyncToDisk(source);
  SyncToDisk(source.parent_path().empty() ? fs::path(".") : source.parent_path());
  const fs::path dest = StringToPath(destFilename);
  SyncToDisk(dest.parent_path().empty() ? fs::path(".") : dest.parent_path());
#endif
  return true;
}

bool CopyRegularFile(std::string_view source_path, std::string_view destination_path)
{
  DEBUG_LOG_FMT(COMMON, "CopyRegularFile: {} --> {}", source_path, destination_path);
  std::error_code ec;
  const bool copied = fs::copy_file(StringToPath(source_path), StringToPath(destination_path),
                                    fs::copy_options::overwrite_existing, ec);
  if (!copied)
  {
    ERROR_LOG_FMT(COMMON, "CopyRegularFile: failed {} --> {}: {}", source_path, destination_path,
                  ec.message());
  }
  return copied;
}

bool Copy(std::string_view source_path, std::string_view dest_path, bool overwrite_existing)
{
  const char* const mode = overwrite_existing ? "overwrite" : "preserve";
  DEBUG_LOG_FMT(COMMON, "Copy: {} --> {} ({})", source_path, dest_path, mode);

  const fs::path source = StringToPath(source_path);
  const fs::path dest = StringToPath(dest_path);
  fs::copy_options options = fs::copy_options::recursive;
  if (overwrite_existing)
    options |= fs::copy_options::overwrite_existing;

  std::error_code ec;
  fs::copy(source, dest, options, ec);
  if (!ec)
    return true;

  // Copying something onto itself fails, but leaves the destination as wanted.
  std::error_code ignored;
  if (fs::equivalent(source, dest, ignored))
    return true;

  ERROR_LOG_FMT(COMMON, "Copy: failed {} --> {} ({}): {}", source_path, dest_path, mode,
                ec.message());
  return false;
}

namespace
{
bool MergeMove(const fs::path& source, const fs::path& dest, std::error_code& ec)
{
  // The simple case: a rename (fails e.g. across volumes or onto a non-empty directory).
  fs::rename(source, dest, ec);
  if (!ec)
    return true;

  std::error_code status_ec;
  if (!fs::is_directory(source, status_ec))
  {
    // A file: copy it over the destination and remove the original.
    return fs::copy_file(source, dest, fs::copy_options::overwrite_existing, ec) &&
           fs::remove(source, ec);
  }

  // A directory: move its entries one by one, then remove it.
  ec.clear();
  for (fs::directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec))
  {
    if (!MergeMove(it->path(), dest / it->path().filename(), ec))
      return false;
  }
  if (ec)
    return false;
  return fs::remove(source, ec);
}
}  // namespace

bool MoveWithOverwrite(std::string_view source_path, std::string_view dest_path)
{
  DEBUG_LOG_FMT(COMMON, "MoveWithOverwrite: {} --> {}", source_path, dest_path);
  std::error_code ec;
  if (MergeMove(StringToPath(source_path), StringToPath(dest_path), ec))
    return true;
  ERROR_LOG_FMT(COMMON, "MoveWithOverwrite: failed {} --> {}: {}", source_path, dest_path,
                ec.message());
  return false;
}

// ---- Directory listing -----------------------------------------------------------------------

namespace
{
std::string PhysicalName(const fs::path& path)
{
#ifdef _WIN32
  return WStringToUTF8(path.generic_wstring());  // '/' separators
#else
  return PathToString(path);
#endif
}

FSTEntry EntryFor(const fs::directory_entry& dirent)
{
  std::error_code ec;
  FSTEntry entry;
  entry.isDirectory = dirent.is_directory(ec);
  if (!entry.isDirectory && !dirent.is_fifo(ec))
  {
    entry.size = dirent.file_size(ec);
    if (ec)
      entry.size = 0;
  }
  entry.physicalName = PhysicalName(dirent.path());
  entry.virtualName = PathToString(dirent.path().filename());
  return entry;
}

// A directory's size counts its children plus everything below its child directories.
void SumDirectorySize(FSTEntry& dir)
{
  dir.size += dir.children.size();
  for (const FSTEntry& child : dir.children)
  {
    if (child.isDirectory)
      dir.size += child.size;
  }
}

// Fills dir.children; returns false (with ec set) when listing fails, which ends the scan.
bool ListInto(FSTEntry& dir, const fs::path& path, bool recursive, std::error_code& ec)
{
  for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
  {
    dir.children.push_back(EntryFor(*it));
    FSTEntry& child = dir.children.back();

    // Like std::filesystem's recursive iteration, don't descend through directory symlinks.
    std::error_code link_ec;
    if (recursive && child.isDirectory && !it->is_symlink(link_ec))
    {
      const bool ok = ListInto(child, it->path(), true, ec);
      SumDirectorySize(child);
      if (!ok)
        return false;
    }
  }
  return !ec;
}
}  // namespace

FSTEntry ScanDirectoryTree(const std::string& directory, bool recursive)
{
  DEBUG_LOG_FMT(COMMON, "ScanDirectoryTree: directory {}", directory);

  const fs::path root = StringToPath(directory);
  FSTEntry tree;
  tree.physicalName = PhysicalName(root);
  std::error_code dir_ec;
  tree.isDirectory = fs::is_directory(root, dir_ec);

  std::error_code ec;
  ListInto(tree, root, recursive, ec);
  SumDirectorySize(tree);

  // The (possibly partial) listing is still returned.
  if (ec)
    ERROR_LOG_FMT(COMMON, "ScanDirectoryTree error on {}: {}", directory, ec.message());
  return tree;
}

// ---- Process directories ---------------------------------------------------------------------

std::string GetCurrentDir()
{
  std::error_code ec;
  const fs::path current = fs::current_path(ec);
  if (ec)
  {
    ERROR_LOG_FMT(COMMON, "GetCurrentDir failed: {}", ec.message());
    return {};
  }
  return PathToString(current);
}

bool SetCurrentDir(const std::string& directory)
{
  std::error_code ec;
  fs::current_path(StringToPath(directory), ec);
  if (ec)
  {
    ERROR_LOG_FMT(COMMON, "SetCurrentDir failed: {}", ec.message());
    return false;
  }
  return true;
}

std::string CreateTempDir()
{
#ifdef _WIN32
  std::array<wchar_t, MAX_PATH + 1> temp_root{};
  if (GetTempPathW(static_cast<DWORD>(temp_root.size()), temp_root.data()) == 0)
    return "";

  GUID guid;
  if (FAILED(CoCreateGuid(&guid)))
    return "";
  std::array<wchar_t, 40> guid_text{};
  if (StringFromGUID2(guid, guid_text.data(), static_cast<int>(guid_text.size())) == 0)
    return "";

  std::string dir = WStringToUTF8(temp_root.data());
  std::ranges::replace(dir, '\\', '/');
  while (dir.ends_with('/'))
    dir.pop_back();
  dir += '/';
  dir += WStringToUTF8(guid_text.data());
  if (!CreateDir(dir))
    return "";
  return dir;
#else
  const char* base = std::getenv("TMPDIR");
  std::string dir = std::string(base ? base : "/tmp") + "/DolphinWii.XXXXXX";
  if (!mkdtemp(dir.data()))
    return "";
  return dir;
#endif
}

std::string GetTempFilenameForAtomicWrite(std::string path)
{
  std::error_code ec;
  const fs::path absolute = fs::absolute(StringToPath(path), ec);
  if (!ec)
    path = PathToString(absolute);
  path += ".xxx";
  return path;
}

std::string GetExePath()
{
#ifdef _WIN32
  const std::optional<std::wstring> module_path = Common::GetModuleName(nullptr);
  if (!module_path)
    return {};
  std::error_code ec;
  const fs::path absolute = fs::absolute(*module_path, ec);
  if (ec)
    return {};
  return PathToString(absolute);
#else
  std::string target(PATH_MAX, '\0');
  const ssize_t length = readlink("/proc/self/exe", target.data(), target.size());
  if (length <= 0 || static_cast<std::size_t>(length) >= target.size())
    return {};
  target.resize(static_cast<std::size_t>(length));
  return target;
#endif
}

std::string GetExeDirectory()
{
  return PathToString(StringToPath(GetExePath()).parent_path());
}

const std::string& GetSysDirectory()
{
  static const std::string sys_directory = [] {
    // Prefer a Sys folder next to the executable; otherwise the installed data directory.
    std::string dir = GetExeDirectory() + DIR_SEP "Sys" DIR_SEP;
#ifdef DATA_DIR
    if (!IsDirectory(dir))
      dir = DATA_DIR "Sys" DIR_SEP;
#endif
    INFO_LOG_FMT(COMMON, "CreateSysDirectoryPath: Setting to {}", dir);
    return dir;
  }();
  return sys_directory;
}

// ---- User paths ------------------------------------------------------------------------------

namespace
{
std::array<std::string, NUM_PATH_INDICES> s_user_paths;

// How a path follows the one it is derived from.
enum class Follows
{
  // Recomputed whenever its base path is set or recomputed.
  Base,
  // Recomputed only when the whole user directory is set; setting the base alone leaves it
  // alone (it typically has a separate configurable setting).
  UserDirOnly,
};

struct DerivedPath
{
  int index;
  int base;
  const char* name;  // appended to the base path; directories get a trailing '/'
  bool is_directory;
  Follows follows;
};

// Ordered so that every base comes before the paths derived from it.
constexpr DerivedPath DERIVED_PATHS[] = {
    {D_GCUSER_IDX, D_USER_IDX, GC_USER_DIR, true, Follows::Base},
    {D_WIIROOT_IDX, D_USER_IDX, WII_USER_DIR, true, Follows::Base},
    {D_TRIUSER_IDX, D_USER_IDX, TRI_USER_DIR, true, Follows::Base},
    {D_CONFIG_IDX, D_USER_IDX, CONFIG_DIR, true, Follows::Base},
    {D_GAMESETTINGS_IDX, D_USER_IDX, GAMESETTINGS_DIR, true, Follows::Base},
    {D_MAPS_IDX, D_USER_IDX, MAPS_DIR, true, Follows::Base},
    {D_CACHE_IDX, D_USER_IDX, CACHE_DIR, true, Follows::Base},
    {D_COVERCACHE_IDX, D_CACHE_IDX, COVERCACHE_DIR, true, Follows::Base},
    {D_REDUMPCACHE_IDX, D_CACHE_IDX, REDUMPCACHE_DIR, true, Follows::Base},
    {D_SHADERCACHE_IDX, D_CACHE_IDX, SHADERCACHE_DIR, true, Follows::Base},
    {D_RETROACHIEVEMENTSCACHE_IDX, D_CACHE_IDX, RETROACHIEVEMENTSCACHE_DIR, true, Follows::Base},
    {D_SHADERS_IDX, D_USER_IDX, SHADERS_DIR, true, Follows::Base},
    {D_STATESAVES_IDX, D_USER_IDX, STATESAVES_DIR, true, Follows::Base},
    {D_SCREENSHOTS_IDX, D_USER_IDX, SCREENSHOTS_DIR, true, Follows::Base},
    {D_LOAD_IDX, D_USER_IDX, LOAD_DIR, true, Follows::Base},
    {D_HIRESTEXTURES_IDX, D_LOAD_IDX, HIRES_TEXTURES_DIR, true, Follows::Base},
    {D_RIIVOLUTION_IDX, D_LOAD_IDX, RIIVOLUTION_DIR, true, Follows::Base},
    {D_DUMP_IDX, D_USER_IDX, DUMP_DIR, true, Follows::Base},
    {D_DUMPFRAMES_IDX, D_DUMP_IDX, DUMP_FRAMES_DIR, true, Follows::Base},
    {D_DUMPOBJECTS_IDX, D_DUMP_IDX, DUMP_OBJECTS_DIR, true, Follows::Base},
    {D_DUMPAUDIO_IDX, D_DUMP_IDX, DUMP_AUDIO_DIR, true, Follows::Base},
    {D_DUMPTEXTURES_IDX, D_DUMP_IDX, DUMP_TEXTURES_DIR, true, Follows::Base},
    {D_DUMPDSP_IDX, D_DUMP_IDX, DUMP_DSP_DIR, true, Follows::Base},
    {D_DUMPSSL_IDX, D_DUMP_IDX, DUMP_SSL_DIR, true, Follows::Base},
    {D_DUMPDEBUG_IDX, D_DUMP_IDX, DUMP_DEBUG_DIR, true, Follows::Base},
    {D_DUMPDEBUG_BRANCHWATCH_IDX, D_DUMPDEBUG_IDX, DUMP_DEBUG_BRANCHWATCH_DIR, true,
     Follows::Base},
    {D_DUMPDEBUG_JITBLOCKS_IDX, D_DUMPDEBUG_IDX, DUMP_DEBUG_JITBLOCKS_DIR, true, Follows::Base},
    {D_LOGS_IDX, D_USER_IDX, LOGS_DIR, true, Follows::Base},
    {D_THEMES_IDX, D_USER_IDX, THEMES_DIR, true, Follows::Base},
    {D_STYLES_IDX, D_USER_IDX, STYLES_DIR, true, Follows::Base},
    {D_PIPES_IDX, D_USER_IDX, PIPES_DIR, true, Follows::Base},
    {D_WFSROOT_IDX, D_USER_IDX, WFSROOT_DIR, true, Follows::Base},
    {D_BACKUP_IDX, D_USER_IDX, BACKUP_DIR, true, Follows::Base},
    {D_RESOURCEPACK_IDX, D_USER_IDX, RESOURCEPACK_DIR, true, Follows::Base},
    {D_DYNAMICINPUT_IDX, D_LOAD_IDX, DYNAMICINPUT_DIR, true, Follows::Base},
    {D_GRAPHICSMOD_IDX, D_LOAD_IDX, GRAPHICSMOD_DIR, true, Follows::Base},
    {D_BANNERS_WIIROOT_IDX, D_LOAD_IDX, WIIBANNERS_DIR, true, Follows::Base},
    {D_FIRMWARE_IDX, D_LOAD_IDX, FIRMWARE_DIR, true, Follows::UserDirOnly},
    {D_WIISYSCONF_IDX, D_WIIROOT_IDX, WII_SYSCONF_DIR, true, Follows::UserDirOnly},
    {D_WIISDCARDSYNCFOLDER_IDX, D_LOAD_IDX, WIISDSYNC_DIR, true, Follows::UserDirOnly},

    {F_DOLPHINCONFIG_IDX, D_CONFIG_IDX, DOLPHIN_CONFIG, false, Follows::Base},
    {F_GCPADCONFIG_IDX, D_CONFIG_IDX, GCPAD_CONFIG, false, Follows::Base},
    {F_WIIPADCONFIG_IDX, D_CONFIG_IDX, WIIPAD_CONFIG, false, Follows::Base},
    {F_GCKEYBOARDCONFIG_IDX, D_CONFIG_IDX, GCKEYBOARD_CONFIG, false, Follows::Base},
    {F_GFXCONFIG_IDX, D_CONFIG_IDX, GFX_CONFIG, false, Follows::Base},
    {F_LOGGERCONFIG_IDX, D_CONFIG_IDX, LOGGER_CONFIG, false, Follows::Base},
    {F_DUALSHOCKUDPCLIENTCONFIG_IDX, D_CONFIG_IDX, DUALSHOCKUDPCLIENT_CONFIG, false,
     Follows::Base},
    {F_FREELOOKCONFIG_IDX, D_CONFIG_IDX, FREELOOK_CONFIG, false, Follows::Base},
    {F_RETROACHIEVEMENTSCONFIG_IDX, D_CONFIG_IDX, RETROACHIEVEMENTS_CONFIG, false, Follows::Base},
    {F_MAINLOG_IDX, D_LOGS_IDX, MAIN_LOG, false, Follows::Base},
    {F_MEM1DUMP_IDX, D_DUMP_IDX, MEM1_DUMP, false, Follows::Base},
    {F_MEM2DUMP_IDX, D_DUMP_IDX, MEM2_DUMP, false, Follows::Base},
    {F_ARAMDUMP_IDX, D_DUMP_IDX, ARAM_DUMP, false, Follows::Base},
    {F_FAKEVMEMDUMP_IDX, D_DUMP_IDX, FAKEVMEM_DUMP, false, Follows::Base},
    {F_GCSRAM_IDX, D_GCUSER_IDX, GC_SRAM, false, Follows::Base},
    {F_WIISDCARDIMAGE_IDX, D_LOAD_IDX, WII_SD_CARD_IMAGE, false, Follows::UserDirOnly},
    {F_WIISYSCONF_IDX, D_WIISYSCONF_IDX, WII_SYSCONF, false, Follows::UserDirOnly},

    {D_MEMORYWATCHER_IDX, D_USER_IDX, MEMORYWATCHER_DIR, true, Follows::Base},
    {F_MEMORYWATCHERLOCATIONS_IDX, D_MEMORYWATCHER_IDX, MEMORYWATCHER_LOCATIONS, false,
     Follows::UserDirOnly},
    {F_MEMORYWATCHERSOCKET_IDX, D_MEMORYWATCHER_IDX, MEMORYWATCHER_SOCKET, false,
     Follows::UserDirOnly},

    {D_GBAUSER_IDX, D_USER_IDX, GBA_USER_DIR, true, Follows::Base},
    {D_GBASAVES_IDX, D_GBAUSER_IDX, GBASAVES_DIR, true, Follows::UserDirOnly},
    {F_GBABIOS_IDX, D_GBAUSER_IDX, GBA_BIOS, false, Follows::UserDirOnly},

    {D_ASM_ROOT_IDX, D_USER_IDX, ASSEMBLY_DIR, true, Follows::Base},
};

void RecomputeDerivedPaths(unsigned int changed)
{
  const bool whole_tree = changed == D_USER_IDX;
  std::array<bool, NUM_PATH_INDICES> updated{};
  updated[changed] = true;

  for (const DerivedPath& rule : DERIVED_PATHS)
  {
    if (!updated[rule.base] || (!whole_tree && rule.follows != Follows::Base))
      continue;
    std::string path = s_user_paths[rule.base] + rule.name;
    if (rule.is_directory)
      path += DIR_SEP;
    s_user_paths[rule.index] = std::move(path);
    updated[rule.index] = true;
  }

  if (whole_tree)
  {
    // Shader caches used to live directly in the user directory; drop that old copy.
    DeleteDirRecursively(s_user_paths[D_USER_IDX] + SHADERCACHE_LEGACY_DIR DIR_SEP);
  }
}
}  // namespace

const std::string& GetUserPath(unsigned int dir_index)
{
  return s_user_paths[dir_index];
}

void SetUserPath(unsigned int dir_index, std::string path)
{
  if (path.empty())
    return;

#ifdef _WIN32
  std::ranges::replace(path, '\\', '/');
#endif

  // Normalise the trailing separator: directories have exactly one, files none.
  while (path.ends_with('/'))
    path.pop_back();
  if (path.empty())
    return;
  if (dir_index < FIRST_FILE_USER_PATH_IDX)
    path += '/';

  s_user_paths[dir_index] = std::move(path);
  RecomputeDerivedPaths(dir_index);
}

std::string GetThemeDir(const std::string& theme_name)
{
  const std::string user_theme = GetUserPath(D_THEMES_IDX) + theme_name + "/";
  if (Exists(user_theme))
    return user_theme;

  const std::string bundled_theme = GetSysDirectory() + THEMES_DIR "/" + theme_name + "/";
  if (Exists(bundled_theme))
    return bundled_theme;

  return GetSysDirectory() + THEMES_DIR "/" DEFAULT_THEME_DIR "/";
}

// ---- Whole-file helpers ----------------------------------------------------------------------

bool WriteStringToFile(const std::string& filename, std::string_view str)
{
  return IOFile(filename, "wb").WriteBytes(str.data(), str.size());
}

bool ReadFileToString(const std::string& filename, std::string& str)
{
  IOFile file(filename, "rb");
  if (!file)
    return false;
  str.resize(file.GetSize());
  return file.ReadArray(str.data(), str.size());
}
}  // namespace File
