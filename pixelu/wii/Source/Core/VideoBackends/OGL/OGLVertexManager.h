// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>

#include "Common/CommonTypes.h"
#include "Common/GL/GLExtensions/GLExtensions.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VertexManagerBase.h"

namespace OGL
{
class StreamBuffer;

// A vertex array object describing one host vertex layout over the shared vertex and index
// stream buffers.
class GLVertexFormat : public NativeVertexFormat
{
public:
  GLVertexFormat(const PortableVertexDeclaration& vtx_decl);
  ~GLVertexFormat() override;

  GLuint GetVAO() const { return m_vao; }

private:
  GLuint m_vao = 0;
};

// Streams vertices, indices, uniforms and texel-buffer data into GL buffers.
class VertexManager final : public VertexManagerBase
{
public:
  VertexManager();
  ~VertexManager() override;

  bool Initialize() override;

  void UploadUtilityUniforms(const void* uniforms, u32 uniforms_size) override;
  bool UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                         u32* out_offset) override;
  bool UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format, u32* out_offset,
                         const void* palette_data, u32 palette_size,
                         TexelBufferFormat palette_format, u32* out_palette_offset) override;

  GLuint GetVertexBufferHandle() const;
  GLuint GetIndexBufferHandle() const;

protected:
  void ResetBuffer(u32 vertex_stride) override;
  void CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices, u32* out_base_vertex,
                    u32* out_base_index) override;
  void UploadUniforms() override;

private:
  void BindTexelView(u32 unit, TexelBufferFormat format);

  std::unique_ptr<StreamBuffer> m_vertices;
  std::unique_ptr<StreamBuffer> m_indices;
  std::unique_ptr<StreamBuffer> m_texels;
  // One buffer texture per texel format, all viewing m_texels.
  std::array<GLuint, NUM_TEXEL_BUFFER_FORMATS> m_texel_views{};
};
}  // namespace OGL
