// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/Network/KD/NWC24Config.h"

namespace IOS::HLE
{
namespace FS
{
class FileSystem;
}
namespace NWC24
{
// A VFF is a small FAT12/FAT16 volume inside a NAND file, used by WiiConnect24 for downloads
// and mail. Only big-endian VFFs are understood.
constexpr u16 SECTOR_SIZE = 512;
constexpr u16 VF_LITTLE_ENDIAN = 0xFFFE;
constexpr u16 VF_BIG_ENDIAN = 0xFEFF;

// Writes (replacing) `filename` inside the VFF at `path`. On any failure after the VFF was
// opened, the VFF itself is deleted.
ErrorCode WriteToVFF(const std::string& path, const std::string& filename,
                     const std::shared_ptr<FS::FileSystem>& fs, std::span<const u8> data);
// Reads exactly out.size() bytes of `filename`.
ErrorCode ReadFromVFF(const std::string& path, const std::string& filename,
                      const std::shared_ptr<FS::FileSystem>& fs, std::vector<u8>& out);
ErrorCode DeleteFileFromVFF(const std::string& path, const std::string& filename,
                            const std::shared_ptr<FS::FileSystem>& fs);

#pragma pack(push, 1)
// First 32 bytes of a VFF file.
struct VFFHeader final
{
  u8 magic[4];
  u16 endianness;  // VF_BIG_ENDIAN or VF_LITTLE_ENDIAN, read as big endian
  u16 unknown_marker;
  u32 volume_size;    // bytes
  u16 cluster_size;   // in units of 16 bytes
  u16 empty;
  u16 unknown;
  u8 padding[14];
};
static_assert(sizeof(VFFHeader) == 32);
#pragma pack(pop)
}  // namespace NWC24
}  // namespace IOS::HLE
