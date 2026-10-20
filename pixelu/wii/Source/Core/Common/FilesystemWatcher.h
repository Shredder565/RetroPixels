// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace wtr
{
inline namespace watcher
{
class watch;
}
}  // namespace wtr

namespace Common
{
// Watches directory trees and reports changes underneath them through the virtual hooks below.
// Paths are reported with '/' separators. The hooks run on the watcher's own thread.
class FilesystemWatcher
{
public:
  FilesystemWatcher();
  virtual ~FilesystemWatcher();

  // Starts watching `path` (no-op if already watched).
  void Watch(const std::string& path);
  void Unwatch(const std::string& path);

private:
  virtual void PathAdded(std::string_view path) {}
  virtual void PathModified(std::string_view path) {}
  virtual void PathRenamed(std::string_view old_path, std::string_view new_path) {}
  virtual void PathDeleted(std::string_view path) {}

  std::map<std::string, std::unique_ptr<wtr::watch>> m_watched_paths;
};
}  // namespace Common
