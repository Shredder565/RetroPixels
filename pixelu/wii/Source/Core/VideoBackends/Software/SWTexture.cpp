// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Software/SWTexture.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace SW
{
namespace
{
// Every level is allocated with this many bytes per texel, whatever its format.
constexpr size_t STORAGE_TEXEL_BYTES = 4;

// A rectangle of texels inside one mip level of a texture's memory.
struct TexelRegion
{
  const TextureConfig& config;
  u32 level;
  int x;
  int y;

  size_t Stride() const { return config.GetMipStride(level); }
  size_t Offset() const
  {
    const size_t texel = AbstractTexture::GetTexelSizeForFormat(config.format);
    return static_cast<size_t>(y) * Stride() + static_cast<size_t>(x) * texel;
  }
};

// Copies a width x height block row by row. The texel size comes from the source's format.
void CopyRows(const TexelRegion& from, const u8* from_data, const TexelRegion& to, u8* to_data,
              int width, int height)
{
  const size_t row_bytes =
      static_cast<size_t>(width) * AbstractTexture::GetTexelSizeForFormat(from.config.format);
  const size_t from_stride = from.Stride();
  const size_t to_stride = to.Stride();
  const u8* in = from_data + from.Offset();
  u8* out = to_data + to.Offset();
  for (int row = 0; row < height; ++row, in += from_stride, out += to_stride)
    std::memcpy(out, in, row_bytes);
}
}  // namespace

SWTexture::SWTexture(const TextureConfig& tex_config) : AbstractTexture(tex_config)
{
  m_data.resize(tex_config.layers);
  for (auto& levels : m_data)
  {
    levels.resize(tex_config.levels);
    for (u32 level = 0; level < tex_config.levels; ++level)
    {
      const u32 width = std::max(tex_config.width >> level, 1u);
      const u32 height = std::max(tex_config.height >> level, 1u);
      levels[level].resize(width * height * STORAGE_TEXEL_BYTES);
    }
  }
}

void SWTexture::CopyRectangleFromTexture(const AbstractTexture* src,
                                         const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                         u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                         u32 dst_layer, u32 dst_level)
{
  const auto* source = static_cast<const SWTexture*>(src);
  CopyRows({src->GetConfig(), src_level, src_rect.left, src_rect.top},
           source->GetData(src_layer, src_level),
           {m_config, dst_level, dst_rect.left, dst_rect.top}, GetData(dst_layer, dst_level),
           src_rect.GetWidth(), src_rect.GetHeight());
}

void SWTexture::ResolveFromTexture(const AbstractTexture*, const MathUtil::Rectangle<int>&, u32,
                                   u32)
{
  // Software textures are never multisampled.
}

void SWTexture::Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer, size_t,
                     u32 layer)
{
  // Uploads are always four bytes per texel; the source rows may be longer than the image.
  u8* out = GetData(layer, level);
  const size_t row_bytes = width * STORAGE_TEXEL_BYTES;
  for (u32 row = 0; row < height; ++row)
  {
    std::memcpy(out + static_cast<size_t>(width * row) * STORAGE_TEXEL_BYTES,
                buffer + static_cast<size_t>(row_length * row) * STORAGE_TEXEL_BYTES, row_bytes);
  }
}

const u8* SWTexture::GetData(u32 layer, u32 level) const
{
  return m_data[layer][level].data();
}

u8* SWTexture::GetData(u32 layer, u32 level)
{
  return m_data[layer][level].data();
}

SWStagingTexture::SWStagingTexture(StagingTextureType type, const TextureConfig& config)
    : AbstractStagingTexture(type, config)
{
  m_map_stride = m_texel_size * config.width;
  m_data.resize(m_map_stride * config.height);
  m_map_pointer = reinterpret_cast<char*>(m_data.data());
}

SWStagingTexture::~SWStagingTexture() = default;

void SWStagingTexture::CopyFromTexture(const AbstractTexture* src,
                                       const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                       u32 src_level, const MathUtil::Rectangle<int>& dst_rect)
{
  const auto* source = static_cast<const SWTexture*>(src);
  CopyRows({src->GetConfig(), src_level, src_rect.left, src_rect.top},
           source->GetData(src_layer, src_level), {m_config, 0, dst_rect.left, dst_rect.top},
           m_data.data(), src_rect.GetWidth(), src_rect.GetHeight());
  m_needs_flush = true;
}

void SWStagingTexture::CopyToTexture(const MathUtil::Rectangle<int>& src_rect, AbstractTexture* dst,
                                     const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                                     u32 dst_level)
{
  auto* target = static_cast<SWTexture*>(dst);
  CopyRows({m_config, 0, src_rect.left, src_rect.top}, m_data.data(),
           {dst->GetConfig(), dst_level, dst_rect.left, dst_rect.top},
           target->GetData(dst_layer, dst_level), src_rect.GetWidth(), src_rect.GetHeight());
  m_needs_flush = true;
}

void SWStagingTexture::Flush()
{
  // Copies complete immediately, so there is never anything to wait for.
  m_needs_flush = false;
}

bool SWStagingTexture::Map()
{
  // The memory stays mapped for the texture's whole lifetime.
  return true;
}

void SWStagingTexture::Unmap()
{
}

std::unique_ptr<SWFramebuffer> SWFramebuffer::Create(SWTexture* color, SWTexture* depth,
                                                     std::vector<AbstractTexture*> extra_colors)
{
  if (!ValidateConfig(color, depth, extra_colors))
    return nullptr;

  // Size, layer and sample counts come from the colour attachment when there is one.
  const SWTexture& reference = color ? *color : *depth;
  const auto format_of = [](const SWTexture* texture) {
    return texture ? texture->GetFormat() : AbstractTextureFormat::Undefined;
  };
  return std::make_unique<SWFramebuffer>(
      color, depth, std::move(extra_colors), format_of(color), format_of(depth),
      reference.GetWidth(), reference.GetHeight(), reference.GetLayers(), reference.GetSamples());
}

SWFramebuffer::SWFramebuffer(AbstractTexture* color, AbstractTexture* depth,
                             std::vector<AbstractTexture*> extra_colors,
                             AbstractTextureFormat color_fmt, AbstractTextureFormat depth_fmt,
                             u32 w, u32 h, u32 layer_count, u32 sample_count)
    : AbstractFramebuffer(color, depth, std::move(extra_colors), color_fmt, depth_fmt, w, h,
                          layer_count, sample_count)
{
}

}  // namespace SW
