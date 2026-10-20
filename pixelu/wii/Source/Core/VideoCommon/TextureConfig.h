// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <functional>

#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"
#include "Common/MathUtil.h"

// Pixel formats a backend texture can have. The numeric values are used as keys by the
// texture pool, so append new formats only before Undefined.
enum class AbstractTextureFormat : u32
{
  RGBA8,
  BGRA8,
  RGB10_A2,
  RGBA16F,
  DXT1,
  DXT3,
  DXT5,
  BPTC,
  R16,
  D16,
  D24_S8,
  R32F,
  D32F,
  D32F_S8,
  Undefined
};

// What a CPU-visible staging texture is for.
enum class StagingTextureType
{
  Readback,  // GPU writes, CPU reads
  Upload,    // CPU writes, GPU reads
  Mutable    // GPU writes, CPU may both read (slowly) and write
};

// Usage bits for TextureConfig::flags.
enum AbstractTextureFlag : u32
{
  AbstractTextureFlag_RenderTarget = (1 << 0),  // can be bound as a framebuffer attachment
  AbstractTextureFlag_ComputeImage = (1 << 1),  // can be bound as a compute shader image
};

enum class AbstractTextureType : u8
{
  Texture_2DArray = 0,
  Texture_2D = 1,
  Texture_CubeMap = 2,
};

// Everything a backend needs to know to create a texture.
struct TextureConfig
{
  constexpr TextureConfig() = default;
  constexpr TextureConfig(u32 width_, u32 height_, u32 levels_, u32 layers_, u32 samples_,
                          AbstractTextureFormat format_, u32 flags_, AbstractTextureType type_)
      : width(width_), height(height_), levels(levels_), layers(layers_), samples(samples_),
        format(format_), flags(flags_), type(type_)
  {
  }

  bool operator==(const TextureConfig& o) const;
  // Full-size rectangle at the origin.
  MathUtil::Rectangle<int> GetRect() const;
  // Rectangle of one mip level; each dimension halves per level but never drops below 1.
  MathUtil::Rectangle<int> GetMipRect(u32 level) const;
  // Bytes per row of level 0 (or of a block row, for compressed formats).
  size_t GetStride() const;
  size_t GetMipStride(u32 level) const;

  bool IsMultisampled() const { return samples > 1; }
  bool IsRenderTarget() const { return (flags & AbstractTextureFlag_RenderTarget) != 0; }
  bool IsComputeImage() const { return (flags & AbstractTextureFlag_ComputeImage) != 0; }

  u32 width = 0;
  u32 height = 0;
  u32 levels = 1;
  u32 layers = 1;
  u32 samples = 1;
  AbstractTextureFormat format = AbstractTextureFormat::RGBA8;
  u32 flags = 0;
  AbstractTextureType type = AbstractTextureType::Texture_2DArray;
};

// The texture pool is keyed on this hash, and its value also decides the pool's iteration
// order, so the packing below must not change. Width and height take 16 bits each, levels
// sits at bit 32 and layers at bit 48 (overlapping fields simply OR together), the format at
// bit 50 and the flags at bit 58. Sample count and texture type are not part of the key.
template <>
struct std::hash<TextureConfig>
{
  using argument_type = TextureConfig;
  using result_type = size_t;

  result_type operator()(const argument_type& c) const noexcept
  {
    const auto at = [](auto field, int bit) { return static_cast<u64>(field) << bit; };
    const u64 key = at(c.width, 0) | at(c.height, 16) | at(c.levels, 32) | at(c.layers, 48) |
                    at(c.format, 50) | at(c.flags, 58);
    return std::hash<u64>{}(key);
  }
};

template <>
struct fmt::formatter<AbstractTextureType> : EnumFormatter<AbstractTextureType::Texture_CubeMap>
{
  constexpr formatter() : EnumFormatter({"2D Array", "2D", "Cubemap"}) {}
};
