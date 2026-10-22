// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <vector>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "VideoCommon/TextureConfig.h"

class AbstractTexture;

// A render target made of up to one colour texture (plus optional extra colour textures) and one
// depth texture, all single-mip. Backends derive from this to hold their own target object.
class AbstractFramebuffer
{
public:
  AbstractFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                      std::vector<AbstractTexture*> additional_color_attachments,
                      AbstractTextureFormat color_format, AbstractTextureFormat depth_format,
                      u32 width, u32 height, u32 layers, u32 samples);
  virtual ~AbstractFramebuffer();

  // True when the given textures can be combined into one framebuffer.
  static bool ValidateConfig(const AbstractTexture* color_attachment,
                             const AbstractTexture* depth_attachment,
                             const std::vector<AbstractTexture*>& additional_color_attachments);

  AbstractTexture* GetColorAttachment() const { return m_color_attachment; }
  AbstractTexture* GetDepthAttachment() const { return m_depth_attachment; }
  AbstractTextureFormat GetColorFormat() const { return m_color_format; }
  AbstractTextureFormat GetDepthFormat() const { return m_depth_format; }
  bool HasColorBuffer() const { return m_color_format != AbstractTextureFormat::Undefined; }
  bool HasDepthBuffer() const { return m_depth_format != AbstractTextureFormat::Undefined; }
  u32 GetWidth() const { return m_width; }
  u32 GetHeight() const { return m_height; }
  u32 GetLayers() const { return m_layers; }
  u32 GetSamples() const { return m_samples; }
  MathUtil::Rectangle<int> GetRect() const;

protected:
  AbstractTexture* m_color_attachment;
  AbstractTexture* m_depth_attachment;
  std::vector<AbstractTexture*> m_additional_color_attachments;
  AbstractTextureFormat m_color_format;
  AbstractTextureFormat m_depth_format;
  u32 m_width;
  u32 m_height;
  u32 m_layers;
  u32 m_samples;
};
