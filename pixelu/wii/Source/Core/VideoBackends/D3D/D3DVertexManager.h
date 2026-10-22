// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VertexManagerBase.h"

enum class ShaderAttrib : u32;

namespace DX11
{
// Vertex declaration as a D3D11 input layout. The layout is made on first use,
// since D3D11 wants a vertex shader signature to check it against.
class D3DVertexFormat : public NativeVertexFormat
{
public:
  D3DVertexFormat(const PortableVertexDeclaration& vtx_decl);
  ~D3DVertexFormat() override;
  ID3D11InputLayout* GetInputLayout(const void* vs_bytecode, size_t vs_bytecode_size);

private:
  void AddAttribute(const AttributeFormat& format, ShaderAttrib semantic_index);

  std::array<D3D11_INPUT_ELEMENT_DESC, 32> m_elems{};
  UINT m_num_elems = 0;

  std::atomic<ID3D11InputLayout*> m_layout{nullptr};
};

// Streams vertices and indices through two dynamic buffers used in turn (each
// holds both, vertices first), and constants through one dynamic constant
// buffer per stage.
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
  static constexpr u32 BUFFER_COUNT = 2;
  static constexpr u32 BUFFER_SIZE =
      (VERTEX_STREAM_BUFFER_SIZE + INDEX_STREAM_BUFFER_SIZE) / BUFFER_COUNT;

  // Maps the texel buffer for a write of the given size, starting it over
  // (and discarding it) when the write would not fit.
  bool MapTexelBuffer(u32 required_size, D3D11_MAPPED_SUBRESOURCE& sr);

  // Member order is the release order in reverse.
  ComPtr<ID3D11Buffer> m_stream_buffers[BUFFER_COUNT] = {};
  u32 m_stream_index = 0;
  u32 m_stream_cursor = 0;

  ComPtr<ID3D11Buffer> m_vs_constants = nullptr;
  ComPtr<ID3D11Buffer> m_gs_constants = nullptr;
  ComPtr<ID3D11Buffer> m_ps_constants = nullptr;

  ComPtr<ID3D11Buffer> m_custom_constants = nullptr;
  std::size_t m_custom_constants_size = 0;

  ComPtr<ID3D11Buffer> m_texel_buffer = nullptr;
  std::array<ComPtr<ID3D11ShaderResourceView>, NUM_TEXEL_BUFFER_FORMATS> m_texel_views;
  u32 m_texel_cursor = 0;
};

}  // namespace DX11
