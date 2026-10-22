// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D/D3DVertexManager.h"

#include <cstring>
#include <d3d11_4.h>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonTypes.h"

#include "Core/System.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DBoundingBox.h"
#include "VideoBackends/D3D/D3DGfx.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"

#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"

namespace DX11
{
namespace
{
// A CPU-writable dynamic buffer description.
D3D11_BUFFER_DESC DynamicBufferDesc(u32 size, UINT bind)
{
  D3D11_BUFFER_DESC desc;
  desc.ByteWidth = size;
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = bind;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  desc.MiscFlags = 0;
  desc.StructureByteStride = 0;
  return desc;
}

ComPtr<ID3D11Buffer> CreateConstantBuffer(u32 size)
{
  // Constant buffer sizes are whole 16-byte registers.
  const u32 padded = Common::AlignUp(size, 16u);
  const D3D11_BUFFER_DESC desc = DynamicBufferDesc(padded, D3D11_BIND_CONSTANT_BUFFER);
  ComPtr<ID3D11Buffer> buffer;
  const HRESULT result = D3D::device->CreateBuffer(&desc, nullptr, &buffer);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create shader constant buffer (size={}): {}",
             padded, DX11HRWrap(result));
  if (FAILED(result))
    return nullptr;

  D3DCommon::SetDebugObjectName(buffer.Get(), "constant buffer");
  return buffer;
}

// Replaces the whole contents of a constant buffer.
void WriteConstants(ID3D11Buffer* buffer, const void* data, u32 size)
{
  D3D11_MAPPED_SUBRESOURCE mapped;
  D3D::context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
  std::memcpy(mapped.pData, data, size);
  D3D::context->Unmap(buffer, 0);

  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, size);
}

// A typed view over the whole texel buffer.
ComPtr<ID3D11ShaderResourceView> CreateTexelView(ID3D11Buffer* buffer, TexelBufferFormat format,
                                                 DXGI_FORMAT view_format)
{
  D3D11_SHADER_RESOURCE_VIEW_DESC desc;
  desc.Format = view_format;
  desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  desc.BufferEx.FirstElement = 0;
  desc.BufferEx.NumElements =
      VertexManager::TEXEL_STREAM_BUFFER_SIZE / VertexManager::GetTexelBufferElementSize(format);
  desc.BufferEx.Flags = 0;

  ComPtr<ID3D11ShaderResourceView> view;
  const HRESULT result = D3D::device->CreateShaderResourceView(buffer, &desc, &view);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create SRV for texel buffer: {}",
             DX11HRWrap(result));
  return view;
}

struct TexelViewFormat
{
  TexelBufferFormat format;
  DXGI_FORMAT dxgi;
};
constexpr std::array<TexelViewFormat, NUM_TEXEL_BUFFER_FORMATS> TEXEL_VIEW_FORMATS = {{
    {TEXEL_BUFFER_FORMAT_R8_UINT, DXGI_FORMAT_R8_UINT},
    {TEXEL_BUFFER_FORMAT_R16_UINT, DXGI_FORMAT_R16_UINT},
    {TEXEL_BUFFER_FORMAT_RGBA8_UINT, DXGI_FORMAT_R8G8B8A8_UINT},
    {TEXEL_BUFFER_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_UINT},
}};
}  // namespace

VertexManager::VertexManager() = default;

VertexManager::~VertexManager() = default;

bool VertexManager::Initialize()
{
  if (!VertexManagerBase::Initialize())
    return false;

  const D3D11_BUFFER_DESC stream_desc =
      DynamicBufferDesc(BUFFER_SIZE, D3D11_BIND_INDEX_BUFFER | D3D11_BIND_VERTEX_BUFFER);
  for (ComPtr<ID3D11Buffer>& stream : m_stream_buffers)
  {
    const HRESULT result = D3D::device->CreateBuffer(&stream_desc, nullptr, &stream);
    ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create buffer: {}", DX11HRWrap(result));
    if (stream)
      D3DCommon::SetDebugObjectName(stream.Get(), "Buffer of VertexManager");
  }

  m_vs_constants = CreateConstantBuffer(sizeof(VertexShaderConstants));
  m_gs_constants = CreateConstantBuffer(sizeof(GeometryShaderConstants));
  m_ps_constants = CreateConstantBuffer(sizeof(PixelShaderConstants));
  if (!m_vs_constants || !m_gs_constants || !m_ps_constants)
    return false;

  const D3D11_BUFFER_DESC texel_desc =
      DynamicBufferDesc(TEXEL_STREAM_BUFFER_SIZE, D3D11_BIND_SHADER_RESOURCE);
  const HRESULT result = D3D::device->CreateBuffer(&texel_desc, nullptr, &m_texel_buffer);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Creating texel buffer failed: {}", DX11HRWrap(result));
  if (!m_texel_buffer)
    return false;

  for (const TexelViewFormat& entry : TEXEL_VIEW_FORMATS)
  {
    ComPtr<ID3D11ShaderResourceView>& view = m_texel_views[entry.format];
    view = CreateTexelView(m_texel_buffer.Get(), entry.format, entry.dxgi);
    if (!view)
      return false;
  }
  return true;
}

void VertexManager::UploadUtilityUniforms(const void* uniforms, u32 uniforms_size)
{
  // Utility draws put one block of uniforms in the vertex buffer and bind it
  // for all three stages.
  InvalidateConstants();
  WriteConstants(m_vs_constants.Get(), uniforms, uniforms_size);
  D3D::stateman->SetVertexConstants(m_vs_constants.Get());
  D3D::stateman->SetGeometryConstants(m_vs_constants.Get());
  D3D::stateman->SetPixelConstants(m_vs_constants.Get());
}

bool VertexManager::MapTexelBuffer(u32 required_size, D3D11_MAPPED_SUBRESOURCE& sr)
{
  // Appending keeps data earlier draws may still read; starting over discards.
  const bool fits = m_texel_cursor + required_size <= TEXEL_STREAM_BUFFER_SIZE;
  const D3D11_MAP mode = fits ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;
  const HRESULT result = D3D::context->Map(m_texel_buffer.Get(), 0, mode, 0, &sr);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to map texel buffer: {}", DX11HRWrap(result));
  if (FAILED(result))
    return false;

  if (!fits)
    m_texel_cursor = 0;
  return true;
}

bool VertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                      u32* out_offset)
{
  if (data_size > TEXEL_STREAM_BUFFER_SIZE)
    return false;

  const u32 element = GetTexelBufferElementSize(format);
  m_texel_cursor = Common::AlignUp(m_texel_cursor, element);

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (!MapTexelBuffer(data_size, mapped))
    return false;

  u8* const base = static_cast<u8*>(mapped.pData);
  *out_offset = m_texel_cursor / element;
  std::memcpy(base + m_texel_cursor, data, data_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, data_size);
  m_texel_cursor += data_size;

  D3D::context->Unmap(m_texel_buffer.Get(), 0);
  D3D::stateman->SetTexture(0, m_texel_views[static_cast<size_t>(format)].Get());
  return true;
}

bool VertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                      u32* out_offset, const void* palette_data, u32 palette_size,
                                      TexelBufferFormat palette_format, u32* out_palette_offset)
{
  const u32 element = GetTexelBufferElementSize(format);
  const u32 palette_element = GetTexelBufferElementSize(palette_format);
  // Room for the palette's alignment padding is reserved as well.
  const u32 reserve = data_size + palette_size + palette_element;
  if (reserve > TEXEL_STREAM_BUFFER_SIZE)
    return false;

  m_texel_cursor = Common::AlignUp(m_texel_cursor, element);

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (!MapTexelBuffer(reserve, mapped))
    return false;

  // The palette follows the data, aligned to its own element size.
  u8* const base = static_cast<u8*>(mapped.pData) + m_texel_cursor;
  const u32 palette_start = Common::AlignUp(data_size, palette_element);
  std::memcpy(base, data, data_size);
  std::memcpy(base + palette_start, palette_data, palette_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, palette_start + palette_size);
  *out_offset = m_texel_cursor / element;
  *out_palette_offset = (m_texel_cursor + palette_start) / palette_element;
  m_texel_cursor += palette_start + palette_size;

  D3D::context->Unmap(m_texel_buffer.Get(), 0);
  D3D::stateman->SetTexture(0, m_texel_views[static_cast<size_t>(format)].Get());
  D3D::stateman->SetTexture(1, m_texel_views[static_cast<size_t>(palette_format)].Get());
  return true;
}

void VertexManager::ResetBuffer(u32 /*vertex_stride*/)
{
  // Vertices are gathered in CPU memory and copied over in CommitBuffer.
  m_base_buffer_pointer = m_cpu_vertex_buffer.data();
  m_cur_buffer_pointer = m_base_buffer_pointer;
  m_end_buffer_pointer = m_base_buffer_pointer + m_cpu_vertex_buffer.size();
  m_index_generator.Start(m_cpu_index_buffer.data());
}

void VertexManager::CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices,
                                 u32* out_base_vertex, u32* out_base_index)
{
  // Indices are 16-bit and follow the vertices, so the vertex block is
  // rounded up to two bytes.
  const u32 vertex_bytes = Common::AlignUp(num_vertices * vertex_stride, sizeof(u16));
  const u32 index_bytes = num_indices * static_cast<u32>(sizeof(u16));
  const u32 total_bytes = vertex_bytes + index_bytes;

  // Vertices must start on a whole vertex for the base-vertex draw.
  u32 start = m_stream_cursor;
  if (vertex_stride > 0)
  {
    const u32 misalignment = start % vertex_stride;
    if (misalignment != 0)
      start += vertex_stride - misalignment;
  }

  // When the batch does not fit, move to the other buffer and discard it.
  D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
  if (start + total_bytes >= BUFFER_SIZE)
  {
    m_stream_index = (m_stream_index + 1) % BUFFER_COUNT;
    start = 0;
    mode = D3D11_MAP_WRITE_DISCARD;
  }

  *out_base_vertex = vertex_stride > 0 ? start / vertex_stride : 0;
  *out_base_index = (start + vertex_bytes) / static_cast<u32>(sizeof(u16));

  ID3D11Buffer* const stream = m_stream_buffers[m_stream_index].Get();
  D3D11_MAPPED_SUBRESOURCE mapped;
  D3D::context->Map(stream, 0, mode, 0, &mapped);
  u8* const dest = static_cast<u8*>(mapped.pData) + start;
  if (vertex_bytes > 0)
    std::memcpy(dest, m_base_buffer_pointer, vertex_bytes);
  if (index_bytes > 0)
    std::memcpy(dest + vertex_bytes, m_cpu_index_buffer.data(), index_bytes);
  D3D::context->Unmap(stream, 0);

  m_stream_cursor = start + total_bytes;

  ADDSTAT(g_stats.this_frame.bytes_vertex_streamed, vertex_bytes);
  ADDSTAT(g_stats.this_frame.bytes_index_streamed, index_bytes);

  D3D::stateman->SetVertexBuffer(stream, vertex_stride, 0);
  D3D::stateman->SetIndexBuffer(stream);
}

void VertexManager::UploadUniforms()
{
  auto& system = Core::System::GetInstance();

  auto& vertex_constants = system.GetVertexShaderManager();
  if (vertex_constants.dirty)
  {
    WriteConstants(m_vs_constants.Get(), &vertex_constants.constants,
                   sizeof(VertexShaderConstants));
    vertex_constants.dirty = false;
  }

  auto& geometry_constants = system.GetGeometryShaderManager();
  if (geometry_constants.dirty)
  {
    WriteConstants(m_gs_constants.Get(), &geometry_constants.constants,
                   sizeof(GeometryShaderConstants));
    geometry_constants.dirty = false;
  }

  auto& pixel_constants = system.GetPixelShaderManager();
  if (pixel_constants.dirty)
  {
    WriteConstants(m_ps_constants.Get(), &pixel_constants.constants, sizeof(PixelShaderConstants));
    pixel_constants.dirty = false;
  }

  if (pixel_constants.custom_constants_dirty)
  {
    // The custom buffer is remade when the data outgrows the last upload.
    const std::size_t custom_size = pixel_constants.custom_constants.size();
    if (m_custom_constants_size < custom_size)
      m_custom_constants = CreateConstantBuffer(static_cast<u32>(custom_size));
    WriteConstants(m_custom_constants.Get(), pixel_constants.custom_constants.data(),
                   static_cast<u32>(custom_size));
    m_custom_constants_size = custom_size;
    pixel_constants.custom_constants_dirty = false;
  }

  // With per-pixel lighting the pixel stage also reads the vertex constants (b1).
  ID3D11Buffer* const lighting =
      g_ActiveConfig.bEnablePixelLighting ? m_vs_constants.Get() : nullptr;
  D3D::stateman->SetPixelConstants(m_ps_constants.Get(), lighting);
  D3D::stateman->SetVertexConstants(m_vs_constants.Get());
  D3D::stateman->SetGeometryConstants(m_gs_constants.Get());
  D3D::stateman->SetCustomConstants(m_custom_constants.Get());
}
}  // namespace DX11
