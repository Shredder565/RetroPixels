// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/WbfsBlob.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"

namespace DiscIO
{
namespace
{
// The disc as presented: a dual-layer Wii disc's worth of 32 KiB sectors.
constexpr u64 DISC_SIZE = u64(143432) * 2 * 0x8000;
constexpr u64 MIN_CLUSTER_SIZE = 0x8000;

constexpr size_t MAX_FILES = 10;

// Drive header (first drive sector of the first file)
constexpr size_t HEADER_SIZE = 0x200;
constexpr size_t HDR_MAGIC = 0x0;          // u32, "WBFS"
constexpr size_t HDR_SECTOR_COUNT = 0x4;   // u32 BE, number of drive sectors
constexpr size_t HDR_SECTOR_SHIFT = 0x8;   // u8, log2 of the drive sector size
constexpr size_t HDR_CLUSTER_SHIFT = 0x9;  // u8, log2 of the WBFS sector size
constexpr size_t HDR_DISC_SLOTS = 0xC;     // u8 per disc slot, non-zero = used

// Each disc slot starts with a copy of the disc header, then its cluster map.
constexpr u64 SLOT_DISC_HEADER_SIZE = 0x100;
}  // namespace

std::unique_ptr<WbfsFileReader> WbfsFileReader::Create(File::DirectIOFile file,
                                                       const std::string& path)
{
  if (!file.IsOpen())
    return nullptr;

  std::vector<Part> parts;
  u64 start = 0;
  const auto add = [&](File::DirectIOFile f) {
    const u64 size = f.GetSize();
    parts.push_back(Part{std::move(f), start, size});
    start += size;
  };
  add(std::move(file));

  // name.wbfs continues in name.wbf1, name.wbf2, ...
  if (path.size() >= 4)
  {
    while (parts.size() < MAX_FILES)
    {
      std::string next = path;
      next.back() = static_cast<char>('0' + parts.size());
      File::DirectIOFile f(next, File::AccessMode::Read);
      if (!f.IsOpen())
        break;
      add(std::move(f));
    }
  }

  std::unique_ptr<WbfsFileReader> reader(new WbfsFileReader(std::move(parts)));
  if (!reader->Parse())
    return nullptr;
  return reader;
}

WbfsFileReader::WbfsFileReader(std::vector<Part> parts) : m_parts(std::move(parts))
{
  for (const Part& part : m_parts)
    m_raw_size += part.size;
}

WbfsFileReader::~WbfsFileReader() = default;

bool WbfsFileReader::Parse()
{
  std::array<u8, HEADER_SIZE> header;
  File::DirectIOFile& first = m_parts[0].file;
  if (!first.OffsetRead(0, header))
    return false;

  u32 magic;
  std::memcpy(&magic, header.data() + HDR_MAGIC, sizeof(magic));
  if (magic != WBFS_MAGIC)
    return false;

  const u32 sector_shift = header[HDR_SECTOR_SHIFT];
  m_cluster_shift = header[HDR_CLUSTER_SHIFT];
  if (sector_shift >= 64 || m_cluster_shift >= 64)
    return false;

  // The files together must be exactly the size the header claims for the drive.
  const u64 sector_size = u64(1) << sector_shift;
  if (m_raw_size != Common::swap32(header.data() + HDR_SECTOR_COUNT) * sector_size)
    return false;

  m_cluster_size = u64(1) << m_cluster_shift;
  if (m_cluster_size < MIN_CLUSTER_SIZE)
    return false;

  // Only the first disc slot is supported.
  if (header[HDR_DISC_SLOTS] == 0)
    return false;

  // The first slot's data sits in the second drive sector.
  const u64 clusters = (DISC_SIZE + m_cluster_size - 1) / m_cluster_size;
  m_cluster_map.assign(clusters, 0);
  first.OffsetRead(sector_size + SLOT_DISC_HEADER_SIZE, Common::AsWritableU8Span(m_cluster_map));
  for (u16& entry : m_cluster_map)
    entry = Common::swap16(entry);
  return true;
}

std::unique_ptr<BlobReader> WbfsFileReader::CopyReader() const
{
  std::vector<Part> parts;
  parts.reserve(m_parts.size());
  for (const Part& part : m_parts)
    parts.push_back(Part{part.file, part.start, part.size});

  std::unique_ptr<WbfsFileReader> copy(new WbfsFileReader(std::move(parts)));
  if (!copy->Parse())
    return nullptr;
  return copy;
}

u64 WbfsFileReader::GetDataSize() const
{
  return DISC_SIZE;
}

bool WbfsFileReader::Read(u64 offset, u64 nbytes, u8* out_ptr)
{
  if (offset + nbytes > GetDataSize())
    return false;

  while (nbytes > 0)
  {
    // Disc offset -> drive offset. Clusters the disc doesn't use map to drive cluster 0.
    const u64 cluster = offset >> m_cluster_shift;
    if (cluster >= m_cluster_map.size())
    {
      ERROR_LOG_FMT(DISCIO, "WBFS: read past the end of the disc");
      return false;
    }
    const u64 in_cluster = offset & (m_cluster_size - 1);
    const u64 drive_offset = u64(m_cluster_map[cluster]) * m_cluster_size + in_cluster;

    auto part = std::find_if(m_parts.begin(), m_parts.end(), [drive_offset](const Part& p) {
      return drive_offset < p.start + p.size;
    });
    if (part == m_parts.end())
    {
      ERROR_LOG_FMT(DISCIO, "WBFS: cluster {} points past the end of the image", cluster);
      return false;
    }

    const u64 in_part = drive_offset - part->start;
    const u64 n = std::min({part->size - in_part, m_cluster_size - in_cluster, nbytes});
    if (!part->file.OffsetRead(in_part, out_ptr, n))
      return false;

    offset += n;
    out_ptr += n;
    nbytes -= n;
  }
  return true;
}

}  // namespace DiscIO
