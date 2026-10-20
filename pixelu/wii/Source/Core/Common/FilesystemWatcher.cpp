// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/FilesystemWatcher.h"

#include <wtr/watcher.hpp>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

namespace Common
{
FilesystemWatcher::FilesystemWatcher() = default;
FilesystemWatcher::~FilesystemWatcher() = default;

void FilesystemWatcher::Watch(const std::string& path)
{
  auto [entry, is_new] = m_watched_paths.try_emplace(path);
  if (!is_new)
    return;

  entry->second = std::make_unique<wtr::watch>(path, [this](wtr::event event) {
    const std::string name = PathToString(event.path_name);

    // Messages about the watcher itself: the text starts with "e" for errors, "w" for warnings.
    if (event.path_type == wtr::event::path_type::watcher)
    {
      if (name.starts_with('e'))
        ERROR_LOG_FMT(COMMON, "Filesystem watcher: '{}'", name);
      else if (name.starts_with('w'))
        WARN_LOG_FMT(COMMON, "Filesystem watcher: '{}'", name);
      return;
    }

    switch (event.effect_type)
    {
    case wtr::event::effect_type::create:
      PathAdded(WithUnifiedPathSeparators(name));
      break;
    case wtr::event::effect_type::modify:
      PathModified(WithUnifiedPathSeparators(name));
      break;
    case wtr::event::effect_type::destroy:
      PathDeleted(WithUnifiedPathSeparators(name));
      break;
    case wtr::event::effect_type::rename:
      // The new name arrives as the associated event.
      if (!event.associated)
      {
        WARN_LOG_FMT(COMMON, "Rename of '{}' reported without its new name", name);
        break;
      }
      PathRenamed(WithUnifiedPathSeparators(name),
                  WithUnifiedPathSeparators(PathToString(event.associated->path_name)));
      break;
    default:
      break;
    }
  });
}

void FilesystemWatcher::Unwatch(const std::string& path)
{
  m_watched_paths.erase(path);
}
}  // namespace Common
