// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Common/GL/GLExtensions/GLExtensions.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"

namespace OGL
{
// A GL texture object. Textures are created and uploaded through the dedicated
// GL_MUTABLE_TEXTURE_INDEX unit so that draw-time bindings stay untouched.
class OGLTexture final : public AbstractTexture
{
public:
  explicit OGLTexture(const TextureConfig& tex_config, std::string_view name);
  ~OGLTexture() override;

  void CopyRectangleFromTexture(const AbstractTexture* src,
                                const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                u32 dst_layer, u32 dst_level) override;
  void ResolveFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& rect,
                          u32 layer, u32 level) override;
  void Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer, size_t buffer_size,
            u32 layer) override;

  GLuint GetGLTextureId() const { return m_id; }

  // The bind target for this texture's type and sample count.
  GLenum GetGLTarget() const
  {
    const bool ms = m_config.IsMultisampled();
    switch (m_config.type)
    {
    case AbstractTextureType::Texture_2D:
      return ms ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
    case AbstractTextureType::Texture_CubeMap:
      return GL_TEXTURE_CUBE_MAP;
    case AbstractTextureType::Texture_2DArray:
      return ms ? GL_TEXTURE_2D_MULTISAMPLE_ARRAY : GL_TEXTURE_2D_ARRAY;
    }
    return GL_TEXTURE_2D_ARRAY;
  }

  // Internal format for a texture format: the sized one when allocating storage, otherwise the
  // one glTexImage* takes (unsized for the 8-bit colour formats).
  static GLenum GetGLInternalFormatForTextureFormat(AbstractTextureFormat format, bool storage);
  GLenum GetGLFormatForImageTexture() const;

private:
  void AllocateStorage(GLenum target, GLenum internal_format);
  void BlitInto(const OGLTexture* src, const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                u32 src_level, const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                u32 dst_level);

  GLuint m_id = 0;
  std::string m_label;
};

// A pixel pack/unpack buffer object used to move texture data between the GPU and the CPU.
// With buffer storage, sync objects and image load/store, the buffer stays mapped for its whole
// life and a fence tracks when the GPU side of a copy is done.
class OGLStagingTexture final : public AbstractStagingTexture
{
public:
  OGLStagingTexture() = delete;
  ~OGLStagingTexture() override;

  void CopyFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& src_rect,
                       u32 src_layer, u32 src_level,
                       const MathUtil::Rectangle<int>& dst_rect) override;
  void CopyToTexture(const MathUtil::Rectangle<int>& src_rect, AbstractTexture* dst,
                     const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                     u32 dst_level) override;

  bool Map() override;
  void Unmap() override;
  void Flush() override;

  static std::unique_ptr<OGLStagingTexture> Create(StagingTextureType type,
                                                   const TextureConfig& config);

private:
  OGLStagingTexture(StagingTextureType type, const TextureConfig& config, GLenum target,
                    GLuint buffer_name, size_t buffer_size, char* map_ptr, size_t map_stride);

  void ReplaceFence();

  GLenum m_bind_target;
  GLuint m_buffer;
  size_t m_size;
  GLsync m_fence = nullptr;
};

// A framebuffer object. The window framebuffer is FBO 0 with no attachments.
class OGLFramebuffer final : public AbstractFramebuffer
{
public:
  OGLFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                 std::vector<AbstractTexture*> additional_color_attachments,
                 AbstractTextureFormat color_format, AbstractTextureFormat depth_format, u32 width,
                 u32 height, u32 layers, u32 samples, GLuint fbo);
  ~OGLFramebuffer() override;

  static std::unique_ptr<OGLFramebuffer>
  Create(OGLTexture* color_attachment, OGLTexture* depth_attachment,
         std::vector<AbstractTexture*> additional_color_attachments);

  GLuint GetFBO() const { return m_fbo; }

  // For the window framebuffer, whose size follows the window.
  void UpdateDimensions(u32 width, u32 height);

private:
  GLuint m_fbo;
};

}  // namespace OGL
