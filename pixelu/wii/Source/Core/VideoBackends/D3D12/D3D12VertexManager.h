// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "VideoBackends/D3D12/D3D12StreamBuffer.h"
#include "VideoBackends/D3D12/DescriptorHeapManager.h"
#include "VideoCommon/VertexManagerBase.h"

namespace DX12
{
// Streams vertices, indices, shader constants and texel-buffer data through
// upload rings and points the Gfx bindings at the newest data.
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

protected:
  void ResetBuffer(u32 vertex_stride) override;
  void CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices, u32* out_base_vertex,
                    u32* out_base_index) override;
  void UploadUniforms() override;

  void UpdateVertexShaderConstants();
  void UpdateGeometryShaderConstants();
  void UpdatePixelShaderConstants();
  void UpdateCustomShaderConstants();

  // Makes room for the largest constant block. When the ring is full the
  // command list is submitted and every block uploaded afresh, and false is
  // returned since nothing is left to do for the caller.
  bool ReserveConstantStorage();
  void UploadAllConstants();

  StreamBuffer m_vertex_stream_buffer;
  StreamBuffer m_index_stream_buffer;
  StreamBuffer m_uniform_stream_buffer;
  StreamBuffer m_texel_stream_buffer;
  std::array<DescriptorHandle, NUM_TEXEL_BUFFER_FORMATS> m_texel_buffer_views = {};
  DescriptorHandle m_vertex_srv = {};
};

}  // namespace DX12
