// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/OGL/OGLTexture.h"

#include <algorithm>
#include <cstdint>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/GL/GLUtil.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/OGL/OGLConfig.h"
#include "VideoBackends/OGL/OGLGfx.h"

#include "VideoCommon/VideoConfig.h"

namespace OGL
{
namespace
{
// Client-side layout of a texture format: the pixel format and component type glTexImage*,
// glReadPixels and friends take. Compressed formats do not use these (they report
// GL_UNSIGNED_BYTE for both).
struct PixelLayout
{
  GLenum format;
  GLenum type;
};

PixelLayout LayoutOf(AbstractTextureFormat format)
{
  switch (format)
  {
  case AbstractTextureFormat::RGBA8:
    return {GL_RGBA, GL_UNSIGNED_BYTE};
  case AbstractTextureFormat::BGRA8:
    return {GL_BGRA, GL_UNSIGNED_BYTE};
  case AbstractTextureFormat::RGB10_A2:
    return {GL_RGB10_A2, GL_UNSIGNED_INT_2_10_10_10_REV};
  case AbstractTextureFormat::RGBA16F:
    return {GL_RGBA16F, GL_HALF_FLOAT};
  case AbstractTextureFormat::R16:
    return {GL_RED, GL_UNSIGNED_SHORT};
  case AbstractTextureFormat::R32F:
    return {GL_RED, GL_FLOAT};
  case AbstractTextureFormat::D16:
    return {GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT};
  case AbstractTextureFormat::D32F:
    return {GL_DEPTH_COMPONENT, GL_FLOAT};
  case AbstractTextureFormat::D24_S8:
    return {GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8};
  case AbstractTextureFormat::D32F_S8:
    return {GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV};
  default:
    return {GL_UNSIGNED_BYTE, GL_UNSIGNED_BYTE};
  }
}

// Persistent staging buffers need buffer storage to map them for good, image load/store for
// the client-mapped-buffer barrier, and sync objects to know when the GPU copy is finished.
bool PersistentStaging()
{
  return g_ogl_config.bSupportsGLBufferStorage && g_ogl_config.bSupportsImageLoadStore &&
         g_ogl_config.bSupportsGLSync;
}

const void* BufferOffset(size_t offset)
{
  return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(offset));
}

void* BufferOffsetMutable(size_t offset)
{
  return reinterpret_cast<void*>(static_cast<std::uintptr_t>(offset));
}

// Attaches every layer (layered rendering) or the single layer of a texture.
void AttachToFramebuffer(GLenum attachment, const OGLTexture* texture)
{
  if (texture->GetConfig().layers > 1)
    glFramebufferTexture(GL_FRAMEBUFFER, attachment, texture->GetGLTextureId(), 0);
  else
    glFramebufferTextureLayer(GL_FRAMEBUFFER, attachment, texture->GetGLTextureId(), 0, 0);
}
}  // namespace

GLenum OGLTexture::GetGLInternalFormatForTextureFormat(AbstractTextureFormat format, bool storage)
{
  switch (format)
  {
  case AbstractTextureFormat::RGBA8:
    return storage ? GL_RGBA8 : GL_RGBA;
  case AbstractTextureFormat::BGRA8:
    return storage ? GL_RGBA8 : GL_BGRA;
  case AbstractTextureFormat::RGB10_A2:
    return GL_RGB10_A2;
  case AbstractTextureFormat::RGBA16F:
    return GL_RGBA16F;
  case AbstractTextureFormat::R16:
    return GL_R16;
  case AbstractTextureFormat::R32F:
    return GL_R32F;
  case AbstractTextureFormat::DXT1:
    return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
  case AbstractTextureFormat::DXT3:
    return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
  case AbstractTextureFormat::DXT5:
    return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
  case AbstractTextureFormat::BPTC:
    return GL_COMPRESSED_RGBA_BPTC_UNORM_ARB;
  case AbstractTextureFormat::D16:
    return GL_DEPTH_COMPONENT16;
  case AbstractTextureFormat::D24_S8:
    return GL_DEPTH24_STENCIL8;
  case AbstractTextureFormat::D32F:
    return GL_DEPTH_COMPONENT32F;
  case AbstractTextureFormat::D32F_S8:
    return GL_DEPTH32F_STENCIL8;
  default:
    PanicAlertFmt("Unhandled texture format.");
    return storage ? GL_RGBA8 : GL_RGBA;
  }
}

GLenum OGLTexture::GetGLFormatForImageTexture() const
{
  return GetGLInternalFormatForTextureFormat(m_config.format, true);
}

// ---------------------------------------------------------------------------------------------
// OGLTexture

OGLTexture::OGLTexture(const TextureConfig& tex_config, std::string_view name)
    : AbstractTexture(tex_config), m_label(name)
{
  DEBUG_ASSERT_MSG(VIDEO, !tex_config.IsMultisampled() || tex_config.levels == 1,
                   "OpenGL does not support multisampled textures with mip levels");

  const GLenum target = GetGLTarget();
  glGenTextures(1, &m_id);
  glActiveTexture(GL_MUTABLE_TEXTURE_INDEX);
  glBindTexture(target, m_id);

  if (!m_label.empty() && g_backend_info.bSupportsSettingObjectNames)
    glObjectLabel(GL_TEXTURE, m_id, static_cast<GLsizei>(m_label.size()), m_label.c_str());

  glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(m_config.levels - 1));

  const GLenum internal_format = GetGLInternalFormatForTextureFormat(m_config.format, true);
  AllocateStorage(target, internal_format);

  if (!m_config.IsRenderTarget())
    return;

  ASSERT(!IsCompressedFormat(m_config.format));

  // Render targets need every level defined up front; without immutable storage that means
  // specifying each level with no data. (Other textures get their levels from Load.)
  if (g_ogl_config.bSupportsTextureStorage || m_config.IsMultisampled())
    return;

  const PixelLayout layout = LayoutOf(m_config.format);
  for (u32 level = 0; level < m_config.levels; ++level)
  {
    glTexImage3D(target, static_cast<GLint>(level), static_cast<GLint>(internal_format),
                 static_cast<GLsizei>(std::max(m_config.width >> level, 1u)),
                 static_cast<GLsizei>(std::max(m_config.height >> level, 1u)),
                 static_cast<GLsizei>(m_config.layers), 0, layout.format, layout.type, nullptr);
  }
}

void OGLTexture::AllocateStorage(GLenum target, GLenum internal_format)
{
  const auto levels = static_cast<GLsizei>(m_config.levels);
  const auto width = static_cast<GLsizei>(m_config.width);
  const auto height = static_cast<GLsizei>(m_config.height);
  const auto layers = static_cast<GLsizei>(m_config.layers);
  const auto samples = static_cast<GLsizei>(m_config.samples);
  const bool immutable = g_ogl_config.bSupportsTextureStorage;
  const AbstractTextureType type = m_config.type;

  if (immutable && type == AbstractTextureType::Texture_CubeMap)
  {
    glTexStorage2D(target, levels, internal_format, width, height);
    return;
  }

  if (m_config.IsMultisampled())
  {
    ASSERT(g_ogl_config.bSupportsMSAA);
    const bool ms_storage =
        g_ogl_config.SupportedMultisampleTexStorage != MultisampleTexStorageType::TexStorageNone;
    if (type == AbstractTextureType::Texture_2DArray)
    {
      if (ms_storage)
      {
        glTexStorage3DMultisample(target, samples, internal_format, width, height, layers,
                                  GL_FALSE);
      }
      else
      {
        // glTexImage*Multisample is desktop-only.
        ASSERT(!g_ogl_config.bIsES);
        glTexImage3DMultisample(target, samples, internal_format, width, height, layers, GL_FALSE);
      }
    }
    else if (type == AbstractTextureType::Texture_2D)
    {
      if (ms_storage)
      {
        glTexStorage2DMultisample(target, samples, internal_format, width, height, GL_FALSE);
      }
      else
      {
        ASSERT(!g_ogl_config.bIsES);
        glTexImage2DMultisample(target, samples, internal_format, width, height, GL_FALSE);
      }
    }
    else
    {
      ASSERT_MSG(VIDEO, false, "Unsupported multisampled texture type");
    }
    return;
  }

  if (!immutable)
    return;

  if (type == AbstractTextureType::Texture_2DArray)
    glTexStorage3D(target, levels, internal_format, width, height, layers);
  else if (type == AbstractTextureType::Texture_2D)
    glTexStorage2D(target, levels, internal_format, width, height);
  else
    ASSERT_MSG(VIDEO, false, "Unsupported texture type");
}

OGLTexture::~OGLTexture()
{
  GetOGLGfx()->UnbindTexture(this);
  glDeleteTextures(1, &m_id);
}

void OGLTexture::CopyRectangleFromTexture(const AbstractTexture* src,
                                          const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                          u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                          u32 dst_layer, u32 dst_level)
{
  const auto* source = static_cast<const OGLTexture*>(src);
  ASSERT(src_rect.GetWidth() == dst_rect.GetWidth() &&
         src_rect.GetHeight() == dst_rect.GetHeight());

  if (!g_ogl_config.bSupportsCopySubImage)
  {
    BlitInto(source, src_rect, src_layer, src_level, dst_rect, dst_layer, dst_level);
    return;
  }

  glCopyImageSubData(source->m_id, source->GetGLTarget(), static_cast<GLint>(src_level),
                     src_rect.left, src_rect.top, static_cast<GLint>(src_layer), m_id,
                     GetGLTarget(), static_cast<GLint>(dst_level), dst_rect.left, dst_rect.top,
                     static_cast<GLint>(dst_layer), dst_rect.GetWidth(), dst_rect.GetHeight(), 1);
}

void OGLTexture::BlitInto(const OGLTexture* src, const MathUtil::Rectangle<int>& src_rect,
                          u32 src_layer, u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                          u32 dst_layer, u32 dst_level)
{
  OGLGfx* const gfx = GetOGLGfx();

  // Source and destination each go on one of the scratch framebuffers.
  gfx->BindSharedReadFramebuffer();
  glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, src->m_id,
                            static_cast<GLint>(src_level), static_cast<GLint>(src_layer));
  gfx->BindSharedDrawFramebuffer();
  glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_id,
                            static_cast<GLint>(dst_level), static_cast<GLint>(dst_layer));

  // Blits are scissored, and the scissor test is normally on; it is the only state touched
  // besides the framebuffer binding.
  glDisable(GL_SCISSOR_TEST);
  glBlitFramebuffer(src_rect.left, src_rect.top, src_rect.right, src_rect.bottom, dst_rect.left,
                    dst_rect.top, dst_rect.right, dst_rect.bottom, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glEnable(GL_SCISSOR_TEST);

  gfx->RestoreFramebufferBinding();
}

void OGLTexture::ResolveFromTexture(const AbstractTexture* src,
                                    const MathUtil::Rectangle<int>& rect, u32 layer, u32 level)
{
  const auto* source = static_cast<const OGLTexture*>(src);
  DEBUG_ASSERT(m_config.samples == 1 && m_config.width == source->m_config.width &&
               m_config.height == source->m_config.height && source->m_config.samples > 1);
  DEBUG_ASSERT(rect.left + rect.GetWidth() <= static_cast<int>(source->m_config.width) &&
               rect.top + rect.GetHeight() <= static_cast<int>(source->m_config.height));

  // Blitting from a multisampled texture resolves it.
  BlitInto(source, rect, layer, level, rect, layer, level);
}

void OGLTexture::Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer,
                      size_t buffer_size, u32 layer)
{
  if (level >= m_config.levels)
    PanicAlertFmt("Texture only has {} levels, can't update level {}", m_config.levels, level);
  if (layer >= m_config.layers)
    PanicAlertFmt("Texture only has {} layer, can't update layer {}", m_config.layers, layer);

  const u32 want_width = std::max(1u, m_config.width >> level);
  const u32 want_height = std::max(1u, m_config.height >> level);
  if (width != want_width || height != want_height)
  {
    PanicAlertFmt("Size of level {} must be {}x{}, but {}x{} requested", level, want_width,
                  want_height, width, height);
  }

  const GLenum target = GetGLTarget();
  glActiveTexture(GL_MUTABLE_TEXTURE_INDEX);
  glBindTexture(target, m_id);

  const bool padded_rows = row_length != width;
  if (padded_rows)
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(row_length));

  // With immutable storage the level exists already and is updated; otherwise it is
  // (re)specified. Cube map faces are addressed by target, array layers by z offset.
  const bool update = g_ogl_config.bSupportsTextureStorage;
  const GLenum upload_format = GetGLInternalFormatForTextureFormat(m_config.format, false);
  const auto lvl = static_cast<GLint>(level);
  const auto w = static_cast<GLsizei>(width);
  const auto h = static_cast<GLsizei>(height);
  const auto z = static_cast<GLint>(layer);
  const AbstractTextureType type = m_config.type;
  const bool two_d =
      type == AbstractTextureType::Texture_2D || type == AbstractTextureType::Texture_CubeMap;
  const GLenum plane_target = type == AbstractTextureType::Texture_CubeMap ?
                                  GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer :
                                  target;

  if (IsCompressedFormat(m_config.format))
  {
    const auto size = static_cast<GLsizei>(buffer_size);
    if (two_d && update)
      glCompressedTexSubImage2D(plane_target, lvl, 0, 0, w, h, upload_format, size, buffer);
    else if (two_d)
      glCompressedTexImage2D(plane_target, lvl, upload_format, w, h, 0, size, buffer);
    else if (type == AbstractTextureType::Texture_2DArray && update)
      glCompressedTexSubImage3D(target, lvl, 0, 0, z, w, h, 1, upload_format, size, buffer);
    else if (type == AbstractTextureType::Texture_2DArray)
      glCompressedTexImage3D(target, lvl, upload_format, w, h, 1, 0, size, buffer);
    else
      PanicAlertFmt("Failed to handle compressed texture load - unhandled type");
  }
  else
  {
    const PixelLayout layout = LayoutOf(m_config.format);
    const auto internal = static_cast<GLint>(upload_format);
    if (two_d && update)
      glTexSubImage2D(plane_target, lvl, 0, 0, w, h, layout.format, layout.type, buffer);
    else if (two_d)
      glTexImage2D(plane_target, lvl, internal, w, h, 0, layout.format, layout.type, buffer);
    else if (type == AbstractTextureType::Texture_2DArray && update)
      glTexSubImage3D(target, lvl, 0, 0, z, w, h, 1, layout.format, layout.type, buffer);
    else if (type == AbstractTextureType::Texture_2DArray)
      glTexImage3D(target, lvl, internal, w, h, 1, 0, layout.format, layout.type, buffer);
    else
      PanicAlertFmt("Failed to handle texture load - unhandled type");
  }

  if (padded_rows)
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

// ---------------------------------------------------------------------------------------------
// OGLStagingTexture

OGLStagingTexture::OGLStagingTexture(StagingTextureType type, const TextureConfig& config,
                                     GLenum target, GLuint buffer_name, size_t buffer_size,
                                     char* map_ptr, size_t map_stride)
    : AbstractStagingTexture(type, config), m_bind_target(target), m_buffer(buffer_name),
      m_size(buffer_size)
{
  m_map_pointer = map_ptr;
  m_map_stride = map_stride;
}

OGLStagingTexture::~OGLStagingTexture()
{
  if (m_fence != nullptr)
    glDeleteSync(m_fence);

  if (m_map_pointer != nullptr)
  {
    // Unmapped through the pack binding point, whatever the buffer's own target.
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_buffer);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  }

  if (m_buffer != 0)
    glDeleteBuffers(1, &m_buffer);
}

std::unique_ptr<OGLStagingTexture> OGLStagingTexture::Create(StagingTextureType type,
                                                             const TextureConfig& config)
{
  const size_t stride = config.GetStride();
  const size_t size = stride * config.height;
  const GLenum target =
      type == StagingTextureType::Readback ? GL_PIXEL_PACK_BUFFER : GL_PIXEL_UNPACK_BUFFER;
  const auto gl_size = static_cast<GLsizeiptr>(size);

  GLuint buffer = 0;
  glGenBuffers(1, &buffer);
  glBindBuffer(target, buffer);

  char* mapping = nullptr;
  if (PersistentStaging())
  {
    // Map once, for good. Upload mappings are flushed by hand before each copy.
    GLbitfield access = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
    if (type == StagingTextureType::Readback)
      access = GL_MAP_READ_BIT;
    else if (type == StagingTextureType::Upload)
      access = GL_MAP_WRITE_BIT;
    const GLbitfield storage_flags = access | GL_MAP_PERSISTENT_BIT;
    const GLbitfield map_flags = type == StagingTextureType::Upload ?
                                     storage_flags | GL_MAP_FLUSH_EXPLICIT_BIT :
                                     storage_flags;

    glBufferStorage(target, gl_size, nullptr, storage_flags);
    mapping = static_cast<char*>(glMapBufferRange(target, 0, gl_size, map_flags));
    ASSERT(mapping != nullptr);
  }
  else
  {
    // Mapped on demand in Map().
    glBufferData(target, gl_size, nullptr,
                 type == StagingTextureType::Readback ? GL_STREAM_READ : GL_STREAM_DRAW);
  }
  glBindBuffer(target, 0);

  return std::unique_ptr<OGLStagingTexture>(
      new OGLStagingTexture(type, config, target, buffer, size, mapping, stride));
}

void OGLStagingTexture::ReplaceFence()
{
  if (m_fence != nullptr)
    glDeleteSync(m_fence);
  m_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void OGLStagingTexture::CopyFromTexture(const AbstractTexture* src,
                                        const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                        u32 src_level, const MathUtil::Rectangle<int>& dst_rect)
{
  ASSERT(m_type == StagingTextureType::Readback || m_type == StagingTextureType::Mutable);
  ASSERT(src_rect.GetWidth() == dst_rect.GetWidth() &&
         src_rect.GetHeight() == dst_rect.GetHeight());
  ASSERT(src_rect.left >= 0 && static_cast<u32>(src_rect.right) <= src->GetConfig().width &&
         src_rect.top >= 0 && static_cast<u32>(src_rect.bottom) <= src->GetConfig().height);
  ASSERT(dst_rect.left >= 0 && static_cast<u32>(dst_rect.right) <= m_config.width &&
         dst_rect.top >= 0 && static_cast<u32>(dst_rect.bottom) <= m_config.height);

  const bool persistent = PersistentStaging();

  // A transient mapping must go before the GPU writes into the buffer.
  if (!persistent)
    OGLStagingTexture::Unmap();

  const auto* texture = static_cast<const OGLTexture*>(src);
  const PixelLayout layout = LayoutOf(src->GetFormat());
  const size_t offset = static_cast<size_t>(dst_rect.top) * m_config.GetStride() +
                        static_cast<size_t>(dst_rect.left) * m_texel_size;

  glBindBuffer(GL_PIXEL_PACK_BUFFER, m_buffer);
  glPixelStorei(GL_PACK_ROW_LENGTH, static_cast<GLint>(m_config.width));

  if (g_ogl_config.bSupportsTextureSubImage)
  {
    glGetTextureSubImage(texture->GetGLTextureId(), static_cast<GLint>(src_level), src_rect.left,
                         src_rect.top, static_cast<GLint>(src_layer), src_rect.GetWidth(),
                         src_rect.GetHeight(), 1, layout.format, layout.type,
                         static_cast<GLsizei>(m_size - offset), BufferOffsetMutable(offset));
  }
  else
  {
    // Read through the scratch read framebuffer, with the texture on the colour or the depth
    // attachment and the other one cleared.
    OGLGfx* const gfx = GetOGLGfx();
    gfx->BindSharedReadFramebuffer();
    const GLuint id = texture->GetGLTextureId();
    const auto lvl = static_cast<GLint>(src_level);
    const auto lyr = static_cast<GLint>(src_layer);
    if (AbstractTexture::IsDepthFormat(texture->GetFormat()))
    {
      glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0, 0, 0);
      glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, id, lvl, lyr);
    }
    else
    {
      glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, id, lvl, lyr);
      glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, 0, 0, 0);
    }
    glReadPixels(src_rect.left, src_rect.top, src_rect.GetWidth(), src_rect.GetHeight(),
                 layout.format, layout.type, BufferOffsetMutable(offset));
    gfx->RestoreFramebufferBinding();
  }

  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

  if (persistent)
  {
    // Make the GPU's writes visible to the persistent mapping, and remember when they land.
    if (m_fence != nullptr)
      glDeleteSync(m_fence);
    glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
    m_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
  }

  m_needs_flush = true;
}

void OGLStagingTexture::CopyToTexture(const MathUtil::Rectangle<int>& src_rect,
                                      AbstractTexture* dst,
                                      const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                                      u32 /*dst_level*/)
{
  ASSERT(m_type == StagingTextureType::Upload || m_type == StagingTextureType::Mutable);
  ASSERT(src_rect.GetWidth() == dst_rect.GetWidth() &&
         src_rect.GetHeight() == dst_rect.GetHeight());
  ASSERT(src_rect.left >= 0 && static_cast<u32>(src_rect.right) <= m_config.width &&
         src_rect.top >= 0 && static_cast<u32>(src_rect.bottom) <= m_config.height);
  ASSERT(dst_rect.left >= 0 && static_cast<u32>(dst_rect.right) <= dst->GetConfig().width &&
         dst_rect.top >= 0 && static_cast<u32>(dst_rect.bottom) <= dst->GetConfig().height);

  const bool persistent = PersistentStaging();
  const auto* texture = static_cast<const OGLTexture*>(dst);
  const size_t stride = m_config.GetStride();
  const size_t offset = static_cast<size_t>(src_rect.top) * stride +
                        static_cast<size_t>(src_rect.left) * m_texel_size;
  const size_t length = static_cast<size_t>(src_rect.GetHeight()) * stride;

  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_buffer);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(m_config.width));

  if (persistent)
  {
    // The mapping is not coherent, so the written range is flushed explicitly.
    if (m_type == StagingTextureType::Upload)
    {
      glFlushMappedBufferRange(GL_PIXEL_UNPACK_BUFFER, static_cast<GLintptr>(offset),
                               static_cast<GLsizeiptr>(length));
    }
    glMemoryBarrier(GL_CLIENT_MAPPED_BUFFER_BARRIER_BIT);
  }
  else if (m_map_pointer != nullptr)
  {
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    m_map_pointer = nullptr;
  }

  // Always into mip level 0.
  const GLenum target = texture->GetGLTarget();
  const PixelLayout layout = LayoutOf(dst->GetFormat());
  glActiveTexture(GL_MUTABLE_TEXTURE_INDEX);
  glBindTexture(target, texture->GetGLTextureId());
  glTexSubImage3D(target, 0, dst_rect.left, dst_rect.top, static_cast<GLint>(dst_layer),
                  dst_rect.GetWidth(), dst_rect.GetHeight(), 1, layout.format, layout.type,
                  BufferOffset(offset));
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  if (persistent)
  {
    ReplaceFence();
    glFlush();
  }

  m_needs_flush = true;
}

void OGLStagingTexture::Flush()
{
  // Without a fence (no persistent mapping) the data moves in Map() instead.
  if (m_fence != nullptr)
  {
    glClientWaitSync(m_fence, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(m_fence);
    m_fence = nullptr;
  }
  m_needs_flush = false;
}

bool OGLStagingTexture::Map()
{
  if (m_map_pointer != nullptr)
    return true;

  GLbitfield access = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
  if (m_type == StagingTextureType::Readback)
    access = GL_MAP_READ_BIT;
  else if (m_type == StagingTextureType::Upload)
    access = GL_MAP_WRITE_BIT;

  glBindBuffer(m_bind_target, m_buffer);
  m_map_pointer = static_cast<char*>(
      glMapBufferRange(m_bind_target, 0, static_cast<GLsizeiptr>(m_size), access));
  glBindBuffer(m_bind_target, 0);
  return m_map_pointer != nullptr;
}

void OGLStagingTexture::Unmap()
{
  // Persistent mappings live as long as the buffer.
  if (m_map_pointer == nullptr || PersistentStaging())
    return;

  glBindBuffer(m_bind_target, m_buffer);
  glUnmapBuffer(m_bind_target);
  glBindBuffer(m_bind_target, 0);
  m_map_pointer = nullptr;
}

// ---------------------------------------------------------------------------------------------
// OGLFramebuffer

OGLFramebuffer::OGLFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                               std::vector<AbstractTexture*> additional_color_attachments,
                               AbstractTextureFormat color_format,
                               AbstractTextureFormat depth_format, u32 width, u32 height,
                               u32 layers, u32 samples, GLuint fbo)
    : AbstractFramebuffer(color_attachment, depth_attachment,
                          std::move(additional_color_attachments), color_format, depth_format,
                          width, height, layers, samples),
      m_fbo(fbo)
{
}

OGLFramebuffer::~OGLFramebuffer()
{
  glDeleteFramebuffers(1, &m_fbo);
}

std::unique_ptr<OGLFramebuffer>
OGLFramebuffer::Create(OGLTexture* color_attachment, OGLTexture* depth_attachment,
                       std::vector<AbstractTexture*> additional_color_attachments)
{
  if (!ValidateConfig(color_attachment, depth_attachment, additional_color_attachments))
    return nullptr;

  const OGLTexture* sizing = color_attachment != nullptr ? color_attachment : depth_attachment;
  const AbstractTextureFormat color_format = color_attachment != nullptr ?
                                                 color_attachment->GetFormat() :
                                                 AbstractTextureFormat::Undefined;
  const AbstractTextureFormat depth_format = depth_attachment != nullptr ?
                                                 depth_attachment->GetFormat() :
                                                 AbstractTextureFormat::Undefined;

  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);

  // Colour outputs in order: the main colour attachment, then the additional ones.
  std::vector<GLenum> draw_buffers;
  if (color_attachment != nullptr)
  {
    AttachToFramebuffer(GL_COLOR_ATTACHMENT0, color_attachment);
    draw_buffers.push_back(GL_COLOR_ATTACHMENT0);
  }
  if (depth_attachment != nullptr)
  {
    AttachToFramebuffer(AbstractTexture::IsStencilFormat(depth_format) ?
                            GL_DEPTH_STENCIL_ATTACHMENT :
                            GL_DEPTH_ATTACHMENT,
                        depth_attachment);
  }
  GLenum next_output = GL_COLOR_ATTACHMENT1;
  for (AbstractTexture* extra : additional_color_attachments)
  {
    AttachToFramebuffer(next_output, static_cast<const OGLTexture*>(extra));
    draw_buffers.push_back(next_output);
    ++next_output;
  }
  glDrawBuffers(static_cast<GLsizei>(draw_buffers.size()), draw_buffers.data());

  DEBUG_ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  GetOGLGfx()->RestoreFramebufferBinding();

  return std::make_unique<OGLFramebuffer>(color_attachment, depth_attachment,
                                          std::move(additional_color_attachments), color_format,
                                          depth_format, sizing->GetWidth(), sizing->GetHeight(),
                                          sizing->GetLayers(), sizing->GetSamples(), fbo);
}

void OGLFramebuffer::UpdateDimensions(u32 width, u32 height)
{
  m_width = width;
  m_height = height;
}

}  // namespace OGL
