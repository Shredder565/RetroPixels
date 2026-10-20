// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/CISOBlob.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"

namespace DiscIO
{
std::unique_ptr<CISOFileReader> CISOFileReader::Create(File::DirectIOFile file)
{
  auto header = std::make_unique<CISOHeader>();
  if (!file.OffsetRead(0, Common::AsWritableU8Span(*header)) || header->magic != CISO_MAGIC)
    return nullptr;
  return std::unique_ptr<CISOFileReader>(new CISOFileReader(std::move(file), *header));
}

CISOFileReader::CISOFileReader(File::DirectIOFile file, const CISOHeader& header)
    : m_file(std::move(file)), m_file_size(m_file.GetSize()), m_block_size(header.block_size),
      m_stored_index(CISO_MAP_SIZE, NOT_STORED)
{
  u32 next = 0;
  for (u32 block = 0; block < CISO_MAP_SIZE; ++block)
  {
    if (header.map[block] == 1)
      m_stored_index[block] = next++;
  }
}

std::unique_ptr<BlobReader> CISOFileReader::CopyReader() const
{
  return Create(m_file);
}

u64 CISOFileReader::GetRawSize() const
{
  return m_file_size;
}

u64 CISOFileReader::GetDataSize() const
{
  // The header can describe this many blocks; the real disc may be shorter.
  return u64(CISO_MAP_SIZE) * m_block_size;
}

bool CISOFileReader::Read(u64 offset, u64 nbytes, u8* out_ptr)
{
  if (offset + nbytes > GetDataSize())
    return false;

  while (nbytes > 0)
  {
    const u64 block = offset / m_block_size;
    const u64 in_block = offset % m_block_size;
    const u64 n = std::min<u64>(m_block_size - in_block, nbytes);

    const u32 stored = block < CISO_MAP_SIZE ? m_stored_index[block] : NOT_STORED;
    if (stored == NOT_STORED)
    {
      std::memset(out_ptr, 0, n);
    }
    else
    {
      const u64 file_offset = CISO_HEADER_SIZE + u64(stored) * m_block_size + in_block;
      if (!m_file.OffsetRead(file_offset, out_ptr, n))
        return false;
    }

    offset += n;
    out_ptr += n;
    nbytes -= n;
  }
  return true;
}

}  // namespace DiscIO
