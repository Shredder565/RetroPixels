// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>

#include "Common/CommonTypes.h"
#include "Common/StringUtil.h"

// Indices for GetUserPath()/SetUserPath(). Directory paths end in '/', file paths do not.
enum
{
  D_USER_IDX,
  D_GCUSER_IDX,
  D_WIIROOT_IDX,          // the configured NAND root (User/Wii by default)
  D_TRIUSER_IDX,
  D_SESSION_WIIROOT_IDX,  // NAND root of the running session (may be a temporary copy)
  D_CONFIG_IDX,
  D_GAMESETTINGS_IDX,     // per-game user settings
  D_SKYLANDERS_IDX,

  D_MAPS_IDX,
  D_CACHE_IDX,
  D_COVERCACHE_IDX,
  D_REDUMPCACHE_IDX,
  D_SHADERCACHE_IDX,
  D_RETROACHIEVEMENTSCACHE_IDX,
  D_SHADERS_IDX,
  D_STATESAVES_IDX,
  D_SCREENSHOTS_IDX,
  D_HIRESTEXTURES_IDX,
  D_RIIVOLUTION_IDX,
  D_DUMP_IDX,
  D_DUMPFRAMES_IDX,
  D_DUMPOBJECTS_IDX,
  D_DUMPAUDIO_IDX,
  D_DUMPTEXTURES_IDX,
  D_DUMPDSP_IDX,
  D_DUMPSSL_IDX,
  D_DUMPDEBUG_IDX,
  D_DUMPDEBUG_BRANCHWATCH_IDX,
  D_DUMPDEBUG_JITBLOCKS_IDX,
  D_LOAD_IDX,
  D_LOGS_IDX,
  D_THEMES_IDX,
  D_STYLES_IDX,
  D_PIPES_IDX,
  D_MEMORYWATCHER_IDX,
  D_WFSROOT_IDX,
  D_BACKUP_IDX,
  D_RESOURCEPACK_IDX,
  D_DYNAMICINPUT_IDX,
  D_GRAPHICSMOD_IDX,
  D_FIRMWARE_IDX,
  D_GBAUSER_IDX,
  D_GBASAVES_IDX,
  D_WIISDCARDSYNCFOLDER_IDX,
  D_WIISYSCONF_IDX,
  D_GPU_DRIVERS_EXTRACTED,
  D_GPU_DRIVERS_TMP,
  D_GPU_DRIVERS_HOOKS,
  D_GPU_DRIVERS_FILE_REDIRECT,
  D_ASM_ROOT_IDX,
  D_BANNERS_WIIROOT_IDX,
  FIRST_FILE_USER_PATH_IDX,
  F_DOLPHINCONFIG_IDX = FIRST_FILE_USER_PATH_IDX,
  F_GCPADCONFIG_IDX,
  F_WIIPADCONFIG_IDX,
  F_GCKEYBOARDCONFIG_IDX,
  F_GFXCONFIG_IDX,
  F_LOGGERCONFIG_IDX,
  F_MAINLOG_IDX,
  F_MEM1DUMP_IDX,
  F_MEM2DUMP_IDX,
  F_ARAMDUMP_IDX,
  F_FAKEVMEMDUMP_IDX,
  F_GCSRAM_IDX,
  F_MEMORYWATCHERLOCATIONS_IDX,
  F_MEMORYWATCHERSOCKET_IDX,
  F_WIISDCARDIMAGE_IDX,
  F_WIISYSCONF_IDX,
  F_DUALSHOCKUDPCLIENTCONFIG_IDX,
  F_FREELOOKCONFIG_IDX,
  F_GBABIOS_IDX,
  F_RETROACHIEVEMENTSCONFIG_IDX,
  NUM_PATH_INDICES
};

// All paths taken and returned here are UTF-8 with '/' separators (backslashes are accepted on
// Windows as input).
namespace File
{
// A file or directory found by ScanDirectoryTree().
struct FSTEntry
{
  bool isDirectory = false;
  u64 size = 0;              // file size; for directories, number of entries below it (recursive)
  std::string physicalName;  // full path on disk
  std::string virtualName;   // file name only
  std::vector<FSTEntry> children;
};

// One stat() of a path, for asking several questions about it.
class FileInfo final
{
public:
  explicit FileInfo(const std::string& path);
  explicit FileInfo(const char* path);

  bool Exists() const;
  bool IsDirectory() const;
  // Exists and is not a directory.
  bool IsFile() const;
  // 0 unless IsFile().
  u64 GetSize() const;

private:
  std::filesystem::file_status m_status;
  std::uintmax_t m_size;
  bool m_exists;
};

bool Exists(const std::string& path);
bool IsDirectory(const std::string& path);
bool IsFile(const std::string& path);
// 0 unless `path` is an existing file.
u64 GetSize(const std::string& path);
// Size of an open file; the file position is preserved.
u64 GetSize(FILE* f);

// Each returns true on success or if the directory already exists.
bool CreateDir(const std::string& filename);
bool CreateDirs(std::string_view filename);
// Creates every directory leading up to the file `fullPath` (not the file itself).
bool CreateFullPath(std::string_view fullPath);

enum class IfAbsentBehavior
{
  ConsoleWarning,
  NoConsoleWarning
};

// Deletes a file (not a directory). A missing file counts as success.
bool Delete(const std::string& filename,
            IfAbsentBehavior behavior = IfAbsentBehavior::ConsoleWarning);

// Deletes an empty directory. A missing directory counts as success.
bool DeleteDir(const std::string& filename,
               IfAbsentBehavior behavior = IfAbsentBehavior::ConsoleWarning);

bool Rename(const std::string& srcFilename, const std::string& destFilename);
// Rename(), then flush the file (and on POSIX the affected directories) to disk.
bool RenameSync(const std::string& srcFilename, const std::string& destFilename);

// Copies a single file, replacing an existing destination.
bool CopyRegularFile(std::string_view source_path, std::string_view destination_path);

bool CreateEmptyFile(const std::string& filename);

// The entries under `directory`, optionally recursing into subdirectories.
FSTEntry ScanDirectoryTree(const std::string& directory, bool recursive);

// Deletes a directory and everything in it.
bool DeleteDirRecursively(const std::string& directory);

std::string GetCurrentDir();

// Recursive std::filesystem::copy(). Also succeeds if source and destination are the same file.
bool Copy(std::string_view source_path, std::string_view dest_path,
          bool overwrite_existing = false);

// Moves source_path onto dest_path, merging directories: files that exist in both places are
// replaced, files only in the destination are kept. On failure the move may be partial.
bool MoveWithOverwrite(std::string_view source_path, std::string_view dest_path);

bool SetCurrentDir(const std::string& directory);

// Creates a new, uniquely named directory in the system temp directory; "" on failure.
std::string CreateTempDir();

// A path next to `path` to write to before renaming it over `path`.
std::string GetTempFilenameForAtomicWrite(std::string path);

// The path for a D_*/F_* index. D_USER_IDX must have been set first.
const std::string& GetUserPath(unsigned int dir_index);

// Sets the path for an index and recomputes the paths derived from it.
void SetUserPath(unsigned int dir_index, std::string path);

// The directory of a UI theme: the user's copy, else the bundled one, else the default theme.
std::string GetThemeDir(const std::string& theme_name);

// The bundled read-only data directory ("Sys"), ending in '/'.
const std::string& GetSysDirectory();

#ifdef ANDROID
void SetSysDirectory(const std::string& path);
void SetGpuDriverDirectories(const std::string& path, const std::string& lib_path);
const std::string GetGpuDriverDirectory(unsigned int dir_index);
#endif

#ifdef __APPLE__
std::string GetBundleDirectory();
#endif

// Absolute path of the running executable, and its directory.
std::string GetExePath();
std::string GetExeDirectory();

bool WriteStringToFile(const std::string& filename, std::string_view str);
bool ReadFileToString(const std::string& filename, std::string& str);

// Opens an fstream on a UTF-8 path (std::filesystem::path handles the conversion to the
// platform's native encoding).
template <typename T>
void OpenFStream(T& fstream, const std::string& filename, std::ios_base::openmode openmode)
{
  fstream.open(StringToPath(filename), openmode);
}
}  // namespace File
