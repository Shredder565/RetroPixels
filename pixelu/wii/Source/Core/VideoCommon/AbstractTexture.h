// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "VideoCommon/TextureConfig.h"

struct ImTextureRef;

// A texture owned by a backend. The config is fixed at creation.
class AbstractTexture
{
public:
  explicit AbstractTexture(const TextureConfig& c);
  virtual ~AbstractTexture() = default;

  // Lets a texture be passed straight to Dear ImGui as an ImTextureID / ImTextureRef; the ID is
  // the object's address.
  using imgui_texture_id = unsigned long long;
  operator imgui_texture_id() const { return reinterpret_cast<imgui_texture_id>(this); }
  operator ImTextureRef() const;

  virtual void CopyRectangleFromTexture(const AbstractTexture* src,
                                        const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                        u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                        u32 dst_layer, u32 dst_level) = 0;
  virtual void ResolveFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& rect,
                                  u32 layer, u32 level) = 0;
  virtual void Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer,
                    size_t buffer_size, u32 layer = 0) = 0;

  // Tells the backend that rendering into this texture is done and it will be sampled next
  // (Vulkan uses this for its layout transition). Does nothing by default.
  virtual void FinishedRendering();

  u32 GetWidth() const { return m_config.width; }
  u32 GetHeight() const { return m_config.height; }
  u32 GetLevels() const { return m_config.levels; }
  u32 GetLayers() const { return m_config.layers; }
  u32 GetSamples() const { return m_config.samples; }
  AbstractTextureFormat GetFormat() const { return m_config.format; }
  MathUtil::Rectangle<int> GetRect() const { return m_config.GetRect(); }
  MathUtil::Rectangle<int> GetMipRect(u32 level) const { return m_config.GetMipRect(level); }
  bool IsMultisampled() const { return m_config.IsMultisampled(); }

  // Reads one mip level back and writes it as a PNG.
  bool Save(const std::string& filename, unsigned int level, int compression = 6) const;

  static bool IsCompressedFormat(AbstractTextureFormat format);
  static bool IsDepthFormat(AbstractTextureFormat format);
  static bool IsStencilFormat(AbstractTextureFormat format);
  static bool IsCompatibleDepthAndColorFormats(AbstractTextureFormat depth_format,
                                               AbstractTextureFormat color_format);
  // Bytes per row of row_length texels (rows of 4x4 blocks for block-compressed formats).
  static u32 CalculateStrideForFormat(AbstractTextureFormat format, u32 row_length);
  // Bytes per texel, or per 4x4 block for block-compressed formats.
  static u32 GetTexelSizeForFormat(AbstractTextureFormat format);
  // Block edge in texels: 4 for block-compressed formats, else 1.
  static u32 GetBlockSizeForFormat(AbstractTextureFormat format);

  const TextureConfig& GetConfig() const;

protected:
  const TextureConfig m_config;
};
