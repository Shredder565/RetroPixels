// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// ES helpers for finding things on the NAND and managing the /title and /import trees.

#include "Core/IOS/ES/ES.h"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Core/IOS/ES/Formats.h"
#include "Core/IOS/FS/FileSystemProxy.h"
#include "Core/IOS/Uids.h"

namespace IOS::HLE
{
namespace
{
constexpr FS::Modes TITLE_DIR_MODES{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::Read};
constexpr FS::Modes CONTENT_DIR_MODES{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::None};
constexpr FS::Modes DATA_DIR_MODES{FS::Mode::ReadWrite, FS::Mode::None, FS::Mode::None};

// Reads a TMD through FSCore, so that the time it takes is accounted for.
ES::TMDReader ReadTMD(FSCore& fs, const std::string& path, Ticks ticks)
{
  const auto fd = fs.Open(PID_KERNEL, PID_KERNEL, path, FS::Mode::Read, {}, ticks);
  if (fd.Get() < 0)
    return {};
  const auto fd_value = static_cast<u64>(fd.Get());

  std::vector<u8> bytes(fs.GetFileStatus(fd_value, ticks)->size);
  // Only a zero-byte result counts as a failure here.
  if (fs.Read(fd_value, bytes.data(), static_cast<u32>(bytes.size()), {}, ticks) == 0)
    return {};
  return ES::TMDReader{std::move(bytes)};
}

// Title ID halves appear in paths as exactly eight hex digits.
bool IsTitleIdHalf(const std::string& name)
{
  return name.size() == 8 && std::ranges::all_of(name, Common::IsXDigit);
}

u64 MakeTitleId(const std::string& high, const std::string& low)
{
  const auto high_word = static_cast<u32>(std::stoul(high, nullptr, 16));
  const auto low_word = static_cast<u32>(std::stoul(low, nullptr, 16));
  return u64{high_word} << 32 | low_word;
}

bool IsDirectory(FS::FileSystem* fs, const std::string& path)
{
  return fs->ReadDirectory(PID_KERNEL, PID_KERNEL, path).has_value();
}

// /title and /import hold <high word>/<low word> directories.
std::vector<u64> ListTitleDirectories(FS::FileSystem* fs, const std::string& root)
{
  const auto high_names = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, root);
  if (!high_names)
  {
    ERROR_LOG_FMT(IOS_ES, "{} is not a directory", root);
    return {};
  }

  std::vector<u64> titles;
  for (const std::string& high : *high_names)
  {
    if (!IsTitleIdHalf(high))
      continue;
    const std::string high_dir = fmt::format("{}/{}", root, high);
    const auto low_names = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, high_dir);
    if (!low_names)
      continue;
    for (const std::string& low : *low_names)
    {
      if (IsTitleIdHalf(low) && IsDirectory(fs, fmt::format("{}/{}", high_dir, low)))
        titles.push_back(MakeTitleId(high, low));
    }
  }
  return titles;
}

// Deletes `path` and then each of its parents (but never the root) as long as they are empty
// or unreadable for a reason other than not existing. Stops at the first failed deletion.
bool DeleteDirectoriesIfEmpty(FS::FileSystem* fs, const std::string& path)
{
  std::string dir = path;
  while (true)
  {
    const auto listing = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, dir);
    const bool remove = listing ? listing->empty() : listing.error() != FS::ResultCode::NotFound;
    if (remove && fs->Delete(PID_KERNEL, PID_KERNEL, dir) != FS::ResultCode::Success)
      return false;

    const size_t slash = dir.rfind('/');
    if (slash == 0 || slash == std::string::npos)
      return true;
    dir.resize(slash);
  }
}
}  // namespace

// -----------------------------------------------------------------------------------------------
// Lookups

ES::TMDReader ESCore::FindImportTMD(u64 title_id, Ticks ticks) const
{
  const std::string path = Common::GetImportTitlePath(title_id) + "/content/title.tmd";
  return ReadTMD(m_ios.GetFSCore(), path, ticks);
}

ES::TMDReader ESCore::FindInstalledTMD(u64 title_id, Ticks ticks) const
{
  return ReadTMD(m_ios.GetFSCore(), Common::GetTMDFileName(title_id), ticks);
}

ES::TicketReader ESCore::FindSignedTicket(u64 title_id, std::optional<u8> desired_version) const
{
  const auto fs = m_ios.GetFS();
  const bool want_v1 = desired_version.has_value() && *desired_version == 1;
  auto file = fs->OpenFile(PID_KERNEL, PID_KERNEL,
                           want_v1 ? Common::GetV1TicketFileName(title_id) :
                                     Common::GetTicketFileName(title_id),
                           FS::Mode::Read);
  // Without a requested version, fall back to a v1 ticket file.
  if (!file && !desired_version.has_value())
  {
    file =
        fs->OpenFile(PID_KERNEL, PID_KERNEL, Common::GetV1TicketFileName(title_id), FS::Mode::Read);
  }
  if (!file)
    return {};

  std::vector<u8> bytes(file->GetStatus()->size);
  if (!file->Read(bytes.data(), bytes.size()))
    return {};
  return ES::TicketReader{std::move(bytes)};
}

std::vector<u64> ESCore::GetInstalledTitles() const
{
  return ListTitleDirectories(m_ios.GetFS().get(), "/title");
}

std::vector<u64> ESCore::GetTitleImports() const
{
  return ListTitleDirectories(m_ios.GetFS().get(), "/import");
}

std::vector<u64> ESCore::GetTitlesWithTickets() const
{
  const auto fs = m_ios.GetFS();
  const auto high_names = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, "/ticket");
  if (!high_names)
  {
    ERROR_LOG_FMT(IOS_ES, "/ticket is not a directory");
    return {};
  }

  // /ticket/<high word>/<low word>.tik (or .tv1 for v1 tickets)
  std::vector<u64> titles;
  for (const std::string& high : *high_names)
  {
    if (!IsTitleIdHalf(high))
      continue;
    const auto files = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, "/ticket/" + high);
    if (!files)
      continue;
    for (const std::string& name : *files)
    {
      if (IsDirectory(fs.get(), fmt::format("/ticket/{}/{}", high, name)))
        continue;
      const std::string low = name.substr(0, 8);
      if (!IsTitleIdHalf(low) || (name != low + ".tik" && name != low + ".tv1"))
        continue;
      titles.push_back(MakeTitleId(high, low));
    }
  }
  return titles;
}

std::vector<ES::Content>
ESCore::GetStoredContentsFromTMD(const ES::TMDReader& tmd,
                                 CheckContentHashes check_content_hashes) const
{
  if (!tmd.IsValid())
    return {};

  const auto is_stored = [&](const ES::Content& content) {
    const std::string path = GetContentPath(tmd.GetTitleId(), content);
    if (path.empty())
      return false;
    const auto file = m_ios.GetFS()->OpenFile(PID_KERNEL, PID_KERNEL, path, FS::Mode::Read);
    if (!file)
      return false;
    if (check_content_hashes == CheckContentHashes::No)
      return true;
    std::vector<u8> data(file->GetStatus()->size);
    if (!file->Read(data.data(), data.size()))
      return false;
    return Common::SHA1::CalculateDigest(data) == content.sha1;
  };

  std::vector<ES::Content> stored;
  for (const ES::Content& content : tmd.GetContents())
  {
    if (is_stored(content))
      stored.push_back(content);
  }
  return stored;
}

u32 ESCore::GetSharedContentsCount() const
{
  const auto fs = m_ios.GetFS();
  const auto names = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, "/shared1");
  u32 count = 0;
  for (const std::string& name : *names)
  {
    if (!IsDirectory(fs.get(), "/shared1/" + name) && name.size() == 12 && name.ends_with(".app"))
      ++count;
  }
  return count;
}

std::vector<std::array<u8, 20>> ESCore::GetSharedContents() const
{
  return ES::SharedContentMap{m_ios.GetFSCore()}.GetHashes();
}

std::string ESCore::GetContentPath(const u64 title_id, const ES::Content& content,
                                   Ticks ticks) const
{
  if (!content.IsShared())
    return fmt::format("{}/{:08x}.app", Common::GetTitleContentPath(title_id), content.id);

  const ES::SharedContentMap map{m_ios.GetFSCore()};
  ticks.Add(map.GetTicks());
  return map.GetFilenameFromSHA1(content.sha1).value_or("");
}

// -----------------------------------------------------------------------------------------------
// Imports

bool ESCore::CreateTitleDirectories(u64 title_id, u16 group_id) const
{
  const auto fs = m_ios.GetFS();

  // Both steps run even when the first fails.
  const std::string content_dir = Common::GetTitleContentPath(title_id);
  const FS::ResultCode created =
      fs->CreateFullPath(PID_KERNEL, PID_KERNEL, content_dir + '/', 0, TITLE_DIR_MODES);
  const FS::ResultCode owned =
      fs->SetMetadata(PID_KERNEL, content_dir, PID_KERNEL, PID_KERNEL, 0, CONTENT_DIR_MODES);
  if (created != FS::ResultCode::Success || owned != FS::ResultCode::Success)
  {
    ERROR_LOG_FMT(IOS_ES, "Failed to create or set metadata on content dir for {:016x}", title_id);
    return false;
  }

  // An existing data directory is kept as it is.
  const std::string data_dir = Common::GetTitleDataPath(title_id);
  const auto listing = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, data_dir);
  if (!listing)
  {
    const bool made = listing.error() == FS::ResultCode::NotFound &&
                      fs->CreateDirectory(PID_KERNEL, PID_KERNEL, data_dir, 0, DATA_DIR_MODES) ==
                          FS::ResultCode::Success;
    if (!made)
    {
      ERROR_LOG_FMT(IOS_ES, "Failed to create data dir for {:016x}", title_id);
      return false;
    }
  }

  // The data directory belongs to the title.
  ES::UIDSys uid_sys{m_ios.GetFSCore()};
  const u32 uid = uid_sys.GetOrInsertUIDForTitle(title_id);
  if (fs->SetMetadata(0, data_dir, uid, group_id, 0, DATA_DIR_MODES) != FS::ResultCode::Success)
  {
    ERROR_LOG_FMT(IOS_ES, "Failed to set metadata on data dir for {:016x}", title_id);
    return false;
  }
  return true;
}

bool ESCore::InitImport(const ES::TMDReader& tmd)
{
  const u64 title_id = tmd.GetTitleId();
  if (!CreateTitleDirectories(title_id, tmd.GetGroupId()))
    return false;

  const auto fs = m_ios.GetFS();
  const std::string import_dir = Common::GetImportTitlePath(title_id) + "/content";
  if (fs->CreateFullPath(PID_KERNEL, PID_KERNEL, import_dir + '/', 0, CONTENT_DIR_MODES) !=
      FS::ResultCode::Success)
  {
    ERROR_LOG_FMT(IOS_ES, "InitImport: Failed to create content dir for {:016x}", title_id);
    return false;
  }

  // An update of an installed title starts from its current contents: IOS moves them into
  // /import (only when the installed TMD exists).
  const auto tmd_meta = fs->GetMetadata(PID_KERNEL, PID_KERNEL, Common::GetTMDFileName(title_id));
  if (!tmd_meta || !tmd_meta->is_file)
    return true;

  if (fs->Rename(PID_KERNEL, PID_KERNEL, Common::GetTitleContentPath(title_id), import_dir) !=
      FS::ResultCode::Success)
  {
    ERROR_LOG_FMT(IOS_ES, "InitImport: Failed to move content dir for {:016x}", title_id);
    return false;
  }
  DeleteDirectoriesIfEmpty(fs.get(), import_dir);
  return true;
}

bool ESCore::FinishImport(const ES::TMDReader& tmd)
{
  const auto fs = m_ios.GetFS();
  const u64 title_id = tmd.GetTitleId();
  const std::string import_dir = Common::GetImportTitlePath(title_id) + "/content";

  // Keep the TMD and the contents it lists; anything else (directories included) goes.
  std::unordered_set<std::string> keep{"title.tmd"};
  for (const ES::Content& content : tmd.GetContents())
    keep.insert(fmt::format("{:08x}.app", content.id));

  const auto names = fs->ReadDirectory(PID_KERNEL, PID_KERNEL, import_dir);
  if (!names)
    return false;
  for (const std::string& name : *names)
  {
    const std::string path = fmt::format("{}/{}", import_dir, name);
    if (IsDirectory(fs.get(), path) || !keep.contains(name))
      fs->Delete(PID_KERNEL, PID_KERNEL, path);
  }

  const std::string content_dir = Common::GetTitleContentPath(title_id);
  if (fs->Rename(PID_KERNEL, PID_KERNEL, import_dir, content_dir) != FS::ResultCode::Success)
  {
    ERROR_LOG_FMT(IOS_ES, "FinishImport: Failed to rename import directory to {}", content_dir);
    return false;
  }
  return true;
}

bool ESCore::WriteImportTMD(const ES::TMDReader& tmd)
{
  const auto fs = m_ios.GetFS();
  const std::string staging = "/tmp/title.tmd";
  {
    const auto file = fs->CreateAndOpenFile(PID_KERNEL, PID_KERNEL, staging, CONTENT_DIR_MODES);
    if (!file)
      return false;
    const std::vector<u8>& bytes = tmd.GetBytes();
    if (!file->Write(bytes.data(), bytes.size()))
      return false;
  }
  const std::string destination =
      fmt::format("{}/content/title.tmd", Common::GetImportTitlePath(tmd.GetTitleId()));
  return fs->Rename(PID_KERNEL, PID_KERNEL, staging, destination) == FS::ResultCode::Success;
}

void ESCore::FinishStaleImport(u64 title_id)
{
  // With a TMD in /import the import had got far enough to be completed; otherwise discard it.
  const ES::TMDReader import_tmd = FindImportTMD(title_id);
  if (import_tmd.IsValid())
  {
    FinishImport(import_tmd);
    return;
  }

  const auto fs = m_ios.GetFS();
  const std::string import_root = Common::GetImportTitlePath(title_id);
  fs->Delete(PID_KERNEL, PID_KERNEL, import_root + "/content");
  DeleteDirectoriesIfEmpty(fs.get(), import_root);
  DeleteDirectoriesIfEmpty(fs.get(), Common::GetTitlePath(title_id));
}

void ESCore::FinishAllStaleImports()
{
  for (const u64 title_id : GetTitleImports())
    FinishStaleImport(title_id);
}

// -----------------------------------------------------------------------------------------------
// System files

s32 ESDevice::WriteSystemFile(const std::string& path, std::span<const u8> data, Ticks ticks)
{
  // Write a temporary file in /tmp with the same name, then move it into place.
  FSCore& fs = GetEmulationKernel().GetFSCore();
  const std::string staging = "/tmp/" + PathToFileName(path);

  FS::ResultCode result =
      fs.CreateFile(PID_KERNEL, PID_KERNEL, staging, {},
                    {FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::None}, ticks);
  if (result != FS::ResultCode::Success)
  {
    ERROR_LOG_FMT(IOS_ES, "Failed to create temporary file {}: {}", staging,
                  static_cast<int>(result));
    return FS::ConvertResult(result);
  }

  FSCore::ScopedFd fd = fs.Open(PID_KERNEL, PID_KERNEL, staging, FS::Mode::ReadWrite, {}, ticks);
  if (fd.Get() < 0)
  {
    ERROR_LOG_FMT(IOS_ES, "Failed to open temporary file {}: {}", staging, fd.Get());
    return static_cast<s32>(fd.Get());
  }

  const auto size = static_cast<u32>(data.size());
  if (fs.Write(static_cast<u64>(fd.Get()), data.data(), size, {}, ticks) != static_cast<s32>(size))
  {
    ERROR_LOG_FMT(IOS_ES, "Failed to write to temporary file {}", staging);
    return ES_EIO;
  }

  const s32 closed = fs.Close(static_cast<u64>(fd.Release()), ticks);
  if (closed != IPC_SUCCESS)
  {
    ERROR_LOG_FMT(IOS_ES, "Failed to close temporary file {}", staging);
    return closed;
  }

  result = fs.RenameFile(PID_KERNEL, PID_KERNEL, staging, path, ticks);
  if (result != FS::ResultCode::Success)
  {
    ERROR_LOG_FMT(IOS_ES, "Failed to move launch file to final destination ({}): {}", path,
                  static_cast<int>(result));
    return FS::ConvertResult(result);
  }
  return IPC_SUCCESS;
}
}  // namespace IOS::HLE
