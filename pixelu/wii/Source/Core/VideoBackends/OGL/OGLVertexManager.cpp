// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/OGL/OGLVertexManager.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/GL/GLExtensions/GLExtensions.h"
#include "Common/GL/GLUtil.h"

#include "VideoBackends/OGL/OGLGfx.h"
#include "VideoBackends/OGL/OGLStreamBuffer.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"

#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
namespace
{
// The element buffer binding belongs to the bound VAO. Before touching the index buffer some
// real vertex format has to be bound, or the binding would land in the attributeless VAO (or
// in none).
void EnsureVertexFormatBound()
{
  if (ProgramShaderCache::IsValidVertexFormatBound())
    return;
  ProgramShaderCache::BindVertexFormat(
      static_cast<GLVertexFormat*>(VertexLoaderManager::GetCurrentVertexFormat()));
}

// Internal formats of the buffer textures, by TexelBufferFormat.
constexpr std::array<GLenum, NUM_TEXEL_BUFFER_FORMATS> TEXEL_VIEW_FORMATS = {
    GL_R8UI,     // TEXEL_BUFFER_FORMAT_R8_UINT
    GL_R16UI,    // TEXEL_BUFFER_FORMAT_R16_UINT
    GL_RGBA8UI,  // TEXEL_BUFFER_FORMAT_RGBA8_UINT
    GL_RG32UI,   // TEXEL_BUFFER_FORMAT_R32G32_UINT
};
}  // namespace

VertexManager::VertexManager() = default;

VertexManager::~VertexManager()
{
  if (g_backend_info.bSupportsPaletteConversion)
    glDeleteTextures(static_cast<GLsizei>(m_texel_views.size()), m_texel_views.data());

  // Deleting the index buffer must find a VAO bound.
  EnsureVertexFormatBound();
  m_texels.reset();
  m_indices.reset();
  m_vertices.reset();
}

bool VertexManager::Initialize()
{
  if (!VertexManagerBase::Initialize())
    return false;

  m_vertices = StreamBuffer::Create(GL_ARRAY_BUFFER, VERTEX_STREAM_BUFFER_SIZE);
  m_indices = StreamBuffer::Create(GL_ELEMENT_ARRAY_BUFFER, INDEX_STREAM_BUFFER_SIZE);

  // Shaders that fetch vertices themselves read the vertex stream as SSBO 1.
  if (g_ActiveConfig.UseVSForLinePointExpand() || g_backend_info.bSupportsDynamicVertexLoader)
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_vertices->GetGLBufferId());

  if (!g_backend_info.bSupportsPaletteConversion)
    return true;

  // The texel stream holds palettes and, with GPU texture decoding, raw texture data. GL only
  // promises 64K texels in a buffer texture, so ask for no more than the driver allows.
  GLint driver_limit = 0;
  glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &driver_limit);
  const GLint texel_bytes = std::min(driver_limit, static_cast<GLint>(TEXEL_STREAM_BUFFER_SIZE));
  m_texels = StreamBuffer::Create(GL_TEXTURE_BUFFER, static_cast<u32>(texel_bytes));

  glGenTextures(static_cast<GLsizei>(m_texel_views.size()), m_texel_views.data());
  glActiveTexture(GL_MUTABLE_TEXTURE_INDEX);
  for (u32 format = 0; format < NUM_TEXEL_BUFFER_FORMATS; ++format)
  {
    glBindTexture(GL_TEXTURE_BUFFER, m_texel_views[format]);
    glTexBuffer(GL_TEXTURE_BUFFER, TEXEL_VIEW_FORMATS[format], m_texels->GetGLBufferId());
  }
  return true;
}

void VertexManager::UploadUtilityUniforms(const void* uniforms, u32 uniforms_size)
{
  InvalidateConstants();
  ProgramShaderCache::UploadConstants(uniforms, uniforms_size);
}

void VertexManager::BindTexelView(u32 unit, TexelBufferFormat format)
{
  // The texture decoding shaders read the texel buffers from units 0 and 1; the cached binding
  // for that unit is no longer valid afterwards.
  glActiveTexture(GL_TEXTURE0 + unit);
  glBindTexture(GL_TEXTURE_BUFFER, m_texel_views[static_cast<u32>(format)]);
  GetOGLGfx()->InvalidateTextureBinding(unit);
}

bool VertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                      u32* out_offset)
{
  if (data_size > m_texels->GetSize())
    return false;

  const u32 element = GetTexelBufferElementSize(format);
  const auto [dst, byte_offset] = m_texels->Map(data_size, element);
  std::memcpy(dst, data, data_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, data_size);
  *out_offset = byte_offset / element;
  m_texels->Unmap(data_size);

  BindTexelView(0, format);
  return true;
}

bool VertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                      u32* out_offset, const void* palette_data, u32 palette_size,
                                      TexelBufferFormat palette_format, u32* out_palette_offset)
{
  const u32 element = GetTexelBufferElementSize(format);
  const u32 palette_element = GetTexelBufferElementSize(palette_format);

  // The palette follows the data, aligned to its own element size (which may need up to one
  // element of padding).
  if (data_size + palette_size + palette_element > m_texels->GetSize())
    return false;

  const auto [dst, byte_offset] =
      m_texels->Map(data_size + palette_size + palette_element, element);
  const u32 palette_start = Common::AlignUp(data_size, palette_element);
  const u32 used = palette_start + palette_size;
  std::memcpy(dst, data, data_size);
  std::memcpy(dst + palette_start, palette_data, palette_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, used);
  *out_offset = byte_offset / element;
  *out_palette_offset = (byte_offset + palette_start) / palette_element;
  m_texels->Unmap(used);

  BindTexelView(0, format);
  BindTexelView(1, palette_format);
  return true;
}

GLuint VertexManager::GetVertexBufferHandle() const
{
  return m_vertices->GetGLBufferId();
}

GLuint VertexManager::GetIndexBufferHandle() const
{
  return m_indices->GetGLBufferId();
}

void VertexManager::ResetBuffer(u32 vertex_stride)
{
  EnsureVertexFormatBound();

  // Vertices start on a whole vertex so the base vertex is an integer.
  const auto vertices = m_vertices->Map(MAXVBUFFERSIZE, vertex_stride);
  m_base_buffer_pointer = vertices.first;
  m_cur_buffer_pointer = vertices.first;
  m_end_buffer_pointer = vertices.first + MAXVBUFFERSIZE;

  const auto indices = m_indices->Map(MAXIBUFFERSIZE * sizeof(u16));
  m_index_generator.Start(reinterpret_cast<u16*>(indices.first));
}

void VertexManager::CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices,
                                 u32* out_base_vertex, u32* out_base_index)
{
  const u32 vertex_bytes = num_vertices * vertex_stride;
  const u32 index_bytes = num_indices * static_cast<u32>(sizeof(u16));

  *out_base_vertex = vertex_stride == 0 ? 0 : m_vertices->GetCurrentOffset() / vertex_stride;
  *out_base_index = m_indices->GetCurrentOffset() / static_cast<u32>(sizeof(u16));

  EnsureVertexFormatBound();
  m_vertices->Unmap(vertex_bytes);
  m_indices->Unmap(index_bytes);

  ADDSTAT(g_stats.this_frame.bytes_vertex_streamed, vertex_bytes);
  ADDSTAT(g_stats.this_frame.bytes_index_streamed, index_bytes);
}

void VertexManager::UploadUniforms()
{
  ProgramShaderCache::UploadConstants();
}
}  // namespace OGL
