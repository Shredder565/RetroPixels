// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/AbstractStagingTexture.h"

#include <algorithm>
#include <cstring>

#include "Common/Assert.h"
#include "VideoCommon/AbstractTexture.h"

namespace
{
// Copies `rows` rows of `row_bytes` bytes between two buffers with their own row pitches.
void CopyRows(char* dst, size_t dst_pitch, const char* src, size_t src_pitch, size_t row_bytes,
              int rows)
{
  for (int i = 0; i < rows; ++i)
  {
    std::memcpy(dst, src, row_bytes);
    dst += dst_pitch;
    src += src_pitch;
  }
}
}  // namespace

AbstractStagingTexture::AbstractStagingTexture(StagingTextureType type, const TextureConfig& c)
    : m_type(type), m_config(c), m_texel_size(AbstractTexture::GetTexelSizeForFormat(c.format))
{
}

AbstractStagingTexture::~AbstractStagingTexture() = default;

void AbstractStagingTexture::CopyFromTexture(const AbstractTexture* src, u32 src_layer,
                                             u32 src_level)
{
  const MathUtil::Rectangle<int> from = src->GetConfig().GetMipRect(src_level);
  CopyFromTexture(src, from, src_layer, src_level, m_config.GetRect());
}

void AbstractStagingTexture::CopyToTexture(AbstractTexture* dst, u32 dst_layer, u32 dst_level)
{
  const MathUtil::Rectangle<int> to = dst->GetConfig().GetMipRect(dst_level);
  CopyToTexture(m_config.GetRect(), dst, to, dst_layer, dst_level);
}

void AbstractStagingTexture::ReadTexels(const MathUtil::Rectangle<int>& rect, void* out_ptr,
                                        u32 out_stride)
{
  ASSERT(m_type != StagingTextureType::Upload);
  if (!PrepareForAccess())
    return;

  ASSERT(rect.left >= 0 && rect.top >= 0 && static_cast<u32>(rect.right) <= m_config.width &&
         static_cast<u32>(rect.bottom) <= m_config.height);

  const size_t row_offset = static_cast<size_t>(rect.top) * m_map_stride;
  const size_t col_offset = static_cast<size_t>(rect.left) * m_texel_size;
  const char* src = m_map_pointer + row_offset + col_offset;
  const size_t rows = static_cast<size_t>(rect.GetHeight());

  // Full-width rows with matching pitch: one block copy (padding included).
  const bool full_rows = rect.left == 0 && static_cast<u32>(rect.right) == m_config.width;
  if (full_rows && out_stride == m_map_stride)
  {
    std::memcpy(out_ptr, src, rows * m_map_stride);
    return;
  }

  const size_t row_bytes =
      std::min(static_cast<size_t>(rect.GetWidth()) * m_texel_size, m_map_stride);
  CopyRows(static_cast<char*>(out_ptr), out_stride, src, m_map_stride, row_bytes,
           rect.GetHeight());
}

void AbstractStagingTexture::ReadTexel(u32 x, u32 y, void* out_ptr)
{
  ASSERT(m_type != StagingTextureType::Upload);
  if (!PrepareForAccess())
    return;

  ASSERT(x < m_config.width && y < m_config.height);
  const size_t offset = y * m_map_stride + x * m_texel_size;
  std::memcpy(out_ptr, m_map_pointer + offset, m_texel_size);
}

void AbstractStagingTexture::WriteTexels(const MathUtil::Rectangle<int>& rect, const void* in_ptr,
                                         u32 in_stride)
{
  ASSERT(m_type != StagingTextureType::Readback);
  if (!PrepareForAccess())
    return;

  ASSERT(rect.left >= 0 && rect.top >= 0 && static_cast<u32>(rect.right) <= m_config.width &&
         static_cast<u32>(rect.bottom) <= m_config.height);

  const size_t row_offset = static_cast<size_t>(rect.top) * m_map_stride;
  const size_t col_offset = static_cast<size_t>(rect.left) * m_texel_size;
  char* dst = m_map_pointer + row_offset + col_offset;
  const size_t rows = static_cast<size_t>(rect.GetHeight());

  const bool full_rows = rect.left == 0 && static_cast<u32>(rect.right) == m_config.width;
  if (full_rows && in_stride == m_map_stride)
  {
    std::memcpy(dst, in_ptr, rows * m_map_stride);
    return;
  }

  const size_t row_bytes =
      std::min(static_cast<size_t>(rect.GetWidth()) * m_texel_size, m_map_stride);
  CopyRows(dst, m_map_stride, static_cast<const char*>(in_ptr), in_stride, row_bytes,
           rect.GetHeight());
}

void AbstractStagingTexture::WriteTexel(u32 x, u32 y, const void* in_ptr)
{
  ASSERT(m_type != StagingTextureType::Readback);
  if (!PrepareForAccess())
    return;

  ASSERT(x < m_config.width && y < m_config.height);
  const size_t offset = y * m_map_stride + x * m_texel_size;
  std::memcpy(m_map_pointer + offset, in_ptr, m_texel_size);
}

bool AbstractStagingTexture::PrepareForAccess()
{
  // A pending GPU copy has to land before the CPU looks at the data.
  if (m_needs_flush)
  {
    if (IsMapped())
      Unmap();
    Flush();
  }
  if (IsMapped())
    return true;
  return Map();
}
