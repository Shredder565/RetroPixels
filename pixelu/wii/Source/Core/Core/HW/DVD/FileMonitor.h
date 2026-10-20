// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "DiscIO/Volume.h"

// Logs (to the FILEMON log) which disc file each DVD read lands in.
namespace FileMonitor
{
class FileLogger
{
public:
  FileLogger();
  ~FileLogger();

  void Log(const DiscIO::Volume& volume, const DiscIO::Partition& partition, u64 offset);

private:
  // The last file reported, so consecutive reads of one file log only once.
  DiscIO::Partition m_previous_partition;
  u64 m_previous_file_offset = 0;
};
}  // namespace FileMonitor
