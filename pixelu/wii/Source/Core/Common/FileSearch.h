// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Common
{
// Lists the contents of `directories` (optionally recursively), sorted and without duplicates.
// With an empty `exts`, files and directories are all returned; otherwise only files whose name
// ends in one of `exts` (e.g. ".iso"), compared case-insensitively. Paths use '/' separators.
std::vector<std::string> DoFileSearch(std::span<const std::string_view> directories,
                                      std::span<const std::string_view> exts = {},
                                      bool recursive = false);

// Single directory and/or single extension forms of the above.
inline std::vector<std::string> DoFileSearch(std::string_view directory, std::string_view ext,
                                             bool recursive = false)
{
  const std::string_view directories[] = {directory};
  const std::string_view exts[] = {ext};
  return DoFileSearch(directories, exts, recursive);
}

inline std::vector<std::string> DoFileSearch(std::string_view directory,
                                             std::span<const std::string_view> exts = {},
                                             bool recursive = false)
{
  const std::string_view directories[] = {directory};
  return DoFileSearch(directories, exts, recursive);
}

inline std::vector<std::string> DoFileSearch(std::span<const std::string_view> directories,
                                             std::string_view ext, bool recursive = false)
{
  const std::string_view exts[] = {ext};
  return DoFileSearch(directories, exts, recursive);
}
}  // namespace Common
