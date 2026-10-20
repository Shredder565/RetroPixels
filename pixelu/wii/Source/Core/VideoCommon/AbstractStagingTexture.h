// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "VideoCommon/TextureConfig.h"

class AbstractTexture;

// A CPU-visible buffer for moving texture data between host memory and the GPU. Readback
// textures receive GPU copies and are read by the CPU, upload textures are written by the CPU
// and copied to the GPU, mutable ones allow both.
class AbstractStagingTexture
{
public:
  explicit AbstractStagingTexture(StagingTextureType type, const TextureConfig& c);
  virtual ~AbstractStagingTexture();

  const TextureConfig& GetConfig() const { return m_config; }
  u32 GetWidth() const { return m_config.width; }
  u32 GetHeight() const { return m_config.height; }
  u32 GetLevels() const { return m_config.levels; }
  u32 GetLayers() const { return m_config.layers; }
  u32 GetSamples() const { return m_config.samples; }
  AbstractTextureFormat GetFormat() const { return m_config.format; }
  MathUtil::Rectangle<int> GetRect() const { return m_config.GetRect(); }
  StagingTextureType GetType() const { return m_type; }
  size_t GetTexelSize() const { return m_texel_size; }

  bool IsMapped() const { return m_map_pointer != nullptr; }
  char* GetMappedPointer() const { return m_map_pointer; }
  size_t GetMappedStride() const { return m_map_stride; }

  // GPU texture -> staging copy. Both rectangles must lie inside their textures.
  virtual void CopyFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& src_rect,
                               u32 src_layer, u32 src_level,
                               const MathUtil::Rectangle<int>& dst_rect) = 0;

  // Copies a whole mip level of src into this texture, which must be the same size.
  void CopyFromTexture(const AbstractTexture* src, u32 src_layer = 0, u32 src_level = 0);

  // Staging -> GPU texture copy. Both rectangles must lie inside their textures.
  virtual void CopyToTexture(const MathUtil::Rectangle<int>& src_rect, AbstractTexture* dst,
                             const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                             u32 dst_level) = 0;

  // Copies all of this texture into a mip level of dst of the same size.
  void CopyToTexture(AbstractTexture* dst, u32 dst_layer = 0, u32 dst_level = 0);

  // Makes the contents addressable through GetMappedPointer. Does not wait for the GPU: after a
  // CopyFromTexture, call Flush first. Backends with persistent mappings may do nothing here.
  virtual bool Map() = 0;

  // Ends CPU access (may do nothing with persistent mappings).
  virtual void Unmap() = 0;

  // Completes pending transfers in both directions, submitting GPU work if needed.
  virtual void Flush() = 0;

  // CPU reads of a rectangle or of one texel, after CopyFromTexture. out_stride is the byte
  // distance between rows at out_ptr. Texels outside the copied area are undefined.
  void ReadTexels(const MathUtil::Rectangle<int>& rect, void* out_ptr, u32 out_stride);
  void ReadTexel(u32 x, u32 y, void* out_ptr);

  // CPU writes of a rectangle or of one texel; follow up with CopyToTexture.
  void WriteTexels(const MathUtil::Rectangle<int>& rect, const void* in_ptr, u32 in_stride);
  void WriteTexel(u32 x, u32 y, const void* in_ptr);

protected:
  // Flushes if needed and maps; false when the texture cannot be mapped.
  bool PrepareForAccess();

  const StagingTextureType m_type;
  const TextureConfig m_config;
  const size_t m_texel_size;

  char* m_map_pointer = nullptr;
  size_t m_map_stride = 0;

  bool m_needs_flush = false;
};
