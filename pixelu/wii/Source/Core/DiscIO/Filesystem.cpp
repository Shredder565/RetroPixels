// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/Filesystem.h"

#include <numeric>

namespace DiscIO
{
FileInfo::~FileInfo() = default;

u64 FileInfo::GetTotalSize() const
{
  if (!IsDirectory())
    return GetSize();

  return std::accumulate(begin(), end(), u64{0}, [](u64 total, const FileInfo& child) {
    return total + child.GetTotalSize();
  });
}

FileSystem::~FileSystem() = default;

}  // namespace DiscIO
