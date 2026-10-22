// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/VulkanLoader.h"
#include "VideoCommon/VertexManagerBase.h"

namespace Vulkan
{
class StreamBuffer;

// Streams vertices, indices, shader constants and texel buffer data through four ring
// buffers and points the state tracker at what it wrote.
class VertexManager : public VertexManagerBase
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

private:
  // Reserves room for every constant block. When that needs a submission, all constants are
  // uploaded afresh into the new command buffer and false is returned.
  bool ReserveConstantStorage();
  void UploadAllConstants();
  // Writes one block at the uniform ring's current position and binds it at `binding`.
  void StreamConstants(u32 binding, const void* data, u32 size);
  // Reserves texel ring space, submitting once if needed.
  bool ReserveTexelSpace(u32 size, u32 alignment);

  // Destroyed in reverse order: texel, uniform, index, vertex.
  std::unique_ptr<StreamBuffer> m_vertex_stream_buffer;
  std::unique_ptr<StreamBuffer> m_index_stream_buffer;
  std::unique_ptr<StreamBuffer> m_uniform_stream_buffer;
  std::unique_ptr<StreamBuffer> m_texel_stream_buffer;
  std::array<VkBufferView, NUM_TEXEL_BUFFER_FORMATS> m_texel_views = {};
  // The largest size of the PS, VS and GS blocks together, with alignment.
  u32 m_constants_reserve = 0;
};
}  // namespace Vulkan
