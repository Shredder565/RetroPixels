// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DVD/FileMonitor.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Common/StringUtil.h"

#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

namespace FileMonitor
{
namespace
{
// Streamed audio formats seen on GameCube and Wii discs. Reads of these happen
// constantly while music plays, so they are logged one level lower.
constexpr std::array<std::string_view, 13> AUDIO_EXTENSIONS{
    ".adp", ".adx", ".afc",  ".ast", ".brstm", ".dsp", ".hps",
    ".ogg", ".sad", ".snd", ".song", ".ssm",   ".str",
};

bool LooksLikeAudio(const std::string& path)
{
  std::string ext;
  SplitPath(path, nullptr, nullptr, &ext);
  Common::ToLower(&ext);
  return std::ranges::find(AUDIO_EXTENSIONS, std::string_view{ext}) != AUDIO_EXTENSIONS.end();
}
}  // namespace

FileLogger::FileLogger() = default;
FileLogger::~FileLogger() = default;

void FileLogger::Log(const DiscIO::Volume& volume, const DiscIO::Partition& partition, u64 offset)
{
  const auto* log_manager = Common::Log::LogManager::GetInstance();
  if (!log_manager->IsEnabled(Common::Log::LogType::FILEMON, Common::Log::LogLevel::LWARNING))
    return;

  const DiscIO::FileSystem* fs = volume.GetFileSystem(partition);
  if (fs == nullptr)
    return;

  const std::unique_ptr<DiscIO::FileInfo> info = fs->FindFileInfo(offset);
  if (info == nullptr)
    return;

  const u64 start = info->GetOffset();
  const bool same_as_before = partition == m_previous_partition && start == m_previous_file_offset;
  if (same_as_before)
    return;
  m_previous_partition = partition;
  m_previous_file_offset = start;

  const std::string path = info->GetPath();
  const std::string line =
      fmt::format("{} kB {}", Common::ThousandSeparate(info->GetSize() / 1000, 7), path);

  if (LooksLikeAudio(path))
    INFO_LOG_FMT(FILEMON, "{}", line);
  else
    WARN_LOG_FMT(FILEMON, "{}", line);
}

}  // namespace FileMonitor
