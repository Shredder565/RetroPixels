// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/TGCBlob.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "Common/BitUtils.h"
#include "Common/Swap.h"

namespace DiscIO
{
namespace
{
// Copies the part of [src_offset, src_offset + src_size) that overlaps the read window
// [offset, offset + size) into the output buffer.
void Overlay(u64 offset, u64 size, u8* out, u64 src_offset, u64 src_size, const u8* src)
{
  const u64 begin = std::max(offset, src_offset);
  const u64 end = std::min(offset + size, src_offset + src_size);
  if (begin < end)
    std::memcpy(out + (begin - offset), src + (begin - src_offset), end - begin);
}

void OverlayU32BE(u64 offset, u64 size, u8* out, u64 at, u32 value)
{
  const u8 bytes[4] = {static_cast<u8>(value >> 24), static_cast<u8>(value >> 16),
                       static_cast<u8>(value >> 8), static_cast<u8>(value)};
  Overlay(offset, size, out, at, sizeof(bytes), bytes);
}

constexpr size_t FST_ENTRY_SIZE = 12;
}  // namespace

std::unique_ptr<TGCFileReader> TGCFileReader::Create(File::DirectIOFile file)
{
  TGCHeader header;
  if (!file.OffsetRead(0, Common::AsWritableU8Span(header)) || header.magic != TGC_MAGIC)
    return nullptr;
  return std::unique_ptr<TGCFileReader>(new TGCFileReader(std::move(file)));
}

TGCFileReader::TGCFileReader(File::DirectIOFile file) : m_file(std::move(file))
{
  TGCHeader header{};
  m_file.OffsetRead(0, Common::AsWritableU8Span(header));
  m_file_size = m_file.GetSize();

  m_header_size = Common::swap32(header.tgc_header_size);
  // 32-bit arithmetic on purpose: the format's offsets are 32-bit and may wrap.
  m_disc_dol_offset = Common::swap32(header.dol_real_offset) - m_header_size;
  m_disc_fst_offset = Common::swap32(header.fst_real_offset) - m_header_size;

  m_fst.resize(Common::swap32(header.fst_size));
  if (!m_file.OffsetRead(Common::swap32(header.fst_real_offset), m_fst))
    m_fst.clear();

  if (m_fst.size() < FST_ENTRY_SIZE)
    return;

  // File entries point into the "virtual" file area; move them to where the data is in the
  // presented disc. Overflow is harmless here since the same wrap happens in reverse.
  const u32 shift = Common::swap32(header.file_area_real_offset) -
                    Common::swap32(header.file_area_virtual_offset) - m_header_size;

  const size_t count = std::min<size_t>(Common::swap32(m_fst.data() + 8),
                                        m_fst.size() / FST_ENTRY_SIZE);
  for (size_t i = 0; i < count; ++i)
  {
    u8* entry = m_fst.data() + i * FST_ENTRY_SIZE;
    if (entry[0] != 0)
      continue;  // directory
    const u32 moved = Common::swap32(entry + 4) + shift;
    entry[4] = static_cast<u8>(moved >> 24);
    entry[5] = static_cast<u8>(moved >> 16);
    entry[6] = static_cast<u8>(moved >> 8);
    entry[7] = static_cast<u8>(moved);
  }
}

std::unique_ptr<BlobReader> TGCFileReader::CopyReader() const
{
  return Create(m_file);
}

u64 TGCFileReader::GetDataSize() const
{
  return m_file_size - m_header_size;
}

bool TGCFileReader::Read(u64 offset, u64 nbytes, u8* out_ptr)
{
  if (!m_file.OffsetRead(offset + m_header_size, out_ptr, nbytes))
    return false;

  // Patch the disc header's DOL and FST offsets and the FST itself.
  OverlayU32BE(offset, nbytes, out_ptr, 0x420, m_disc_dol_offset);
  OverlayU32BE(offset, nbytes, out_ptr, 0x424, m_disc_fst_offset);
  Overlay(offset, nbytes, out_ptr, m_disc_fst_offset, m_fst.size(), m_fst.data());
  return true;
}

}  // namespace DiscIO
