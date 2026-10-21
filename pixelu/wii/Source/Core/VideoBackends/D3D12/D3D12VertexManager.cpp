// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D12/D3D12VertexManager.h"

#include <algorithm>
#include <cstring>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "Core/System.h"

#include "VideoBackends/D3D12/D3D12Gfx.h"
#include "VideoBackends/D3D12/D3D12StreamBuffer.h"
#include "VideoBackends/D3D12/DX12Context.h"

#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"

namespace DX12
{
namespace
{
// Constant buffer slots as Gfx numbers them.
constexpr u32 CB_PIXEL = 0;
constexpr u32 CB_VERTEX = 1;
constexpr u32 CB_PIXEL_CUSTOM = 2;
constexpr u32 CB_GEOMETRY = 3;

constexpr u32 CB_ALIGN = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

// Layout used when every block is uploaded together.
constexpr u32 ALL_PIXEL_AT = 0;
constexpr u32 ALL_VERTEX_AT =
    Common::AlignUp(ALL_PIXEL_AT + sizeof(PixelShaderConstants), CB_ALIGN);
constexpr u32 ALL_GEOMETRY_AT =
    Common::AlignUp(ALL_VERTEX_AT + sizeof(VertexShaderConstants), CB_ALIGN);
constexpr u32 ALL_CUSTOM_AT =
    Common::AlignUp(ALL_GEOMETRY_AT + sizeof(GeometryShaderConstants), CB_ALIGN);

constexpr u32 LARGEST_BLOCK =
    static_cast<u32>(std::max({sizeof(PixelShaderConstants), sizeof(VertexShaderConstants),
                               sizeof(GeometryShaderConstants)}));

constexpr DXGI_FORMAT TEXEL_VIEW_FORMATS[NUM_TEXEL_BUFFER_FORMATS] = {
    DXGI_FORMAT_R8_UINT,        // TEXEL_BUFFER_FORMAT_R8_UINT
    DXGI_FORMAT_R16_UINT,       // TEXEL_BUFFER_FORMAT_R16_UINT
    DXGI_FORMAT_R8G8B8A8_UINT,  // TEXEL_BUFFER_FORMAT_RGBA8_UINT
    DXGI_FORMAT_R32G32_UINT,    // TEXEL_BUFFER_FORMAT_R32G32_UINT
};

// A typed view over a whole stream buffer.
void CreateBufferView(ID3D12Resource* buffer, DXGI_FORMAT format, u32 elements,
                      D3D12_CPU_DESCRIPTOR_HANDLE where)
{
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = format;
  desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Buffer.NumElements = elements;
  g_dx_context->GetDevice()->CreateShaderResourceView(buffer, &desc, where);
}

// Binds the next free spot of the ring to a slot and copies a block there.
void StageBlock(StreamBuffer& ring, u32 slot, const void* data, u32 size)
{
  Gfx::GetInstance()->SetConstantBuffer(slot, ring.GetCurrentGPUPointer());
  std::memcpy(ring.GetCurrentHostPointer(), data, size);
  ring.CommitMemory(size);
}
}  // namespace

VertexManager::VertexManager() = default;

VertexManager::~VertexManager() = default;

bool VertexManager::Initialize()
{
  if (!VertexManagerBase::Initialize())
    return false;

  const bool rings_ok = m_vertex_stream_buffer.AllocateBuffer(VERTEX_STREAM_BUFFER_SIZE) &&
                        m_index_stream_buffer.AllocateBuffer(INDEX_STREAM_BUFFER_SIZE) &&
                        m_uniform_stream_buffer.AllocateBuffer(UNIFORM_STREAM_BUFFER_SIZE) &&
                        m_texel_stream_buffer.AllocateBuffer(TEXEL_STREAM_BUFFER_SIZE);
  if (!rings_ok)
  {
    PanicAlertFmt("Failed to allocate streaming buffers");
    return false;
  }

  DescriptorHeapManager& views = g_dx_context->GetDescriptorHeapManager();
  for (u32 n = 0; n < NUM_TEXEL_BUFFER_FORMATS; ++n)
  {
    const auto format = static_cast<TexelBufferFormat>(n);
    DescriptorHandle& handle = m_texel_buffer_views[n];
    if (!views.Allocate(&handle))
    {
      PanicAlertFmt("Failed to allocate descriptor for texel buffer");
      return false;
    }
    CreateBufferView(m_texel_stream_buffer.GetBuffer(), TEXEL_VIEW_FORMATS[n],
                     m_texel_stream_buffer.GetSize() / GetTexelBufferElementSize(format),
                     handle.cpu_handle);
  }

  // Vertex data is also readable as raw words (for line/point expansion).
  if (!views.Allocate(&m_vertex_srv))
  {
    PanicAlertFmt("Failed to allocate descriptor for vertex srv");
    return false;
  }
  CreateBufferView(m_vertex_stream_buffer.GetBuffer(), DXGI_FORMAT_R32_UINT,
                   m_vertex_stream_buffer.GetSize() / sizeof(u32), m_vertex_srv.cpu_handle);

  UploadAllConstants();
  return true;
}

void VertexManager::ResetBuffer(u32 vertex_stride)
{
  constexpr u32 INDEX_BYTES = MAXIBUFFERSIZE * sizeof(u16);
  bool have_vertices = m_vertex_stream_buffer.ReserveMemory(MAXVBUFFERSIZE, vertex_stride);
  bool have_indices = m_index_stream_buffer.ReserveMemory(INDEX_BYTES, sizeof(u16));
  if (!have_vertices || !have_indices)
  {
    // Submitting lets the rings wait on the fences of the work using them.
    WARN_LOG_FMT(VIDEO, "Executing command list while waiting for space in vertex/index buffer");
    Gfx::GetInstance()->ExecuteCommandList(false);

    if (!have_vertices)
      have_vertices = m_vertex_stream_buffer.ReserveMemory(MAXVBUFFERSIZE, vertex_stride);
    if (!have_indices)
      have_indices = m_index_stream_buffer.ReserveMemory(INDEX_BYTES, sizeof(u16));

    // Still failing means the request can never fit.
    if (!have_vertices || !have_indices)
      PanicAlertFmt("Failed to allocate space in streaming buffers for pending draw");
  }

  u8* const vertices = m_vertex_stream_buffer.GetCurrentHostPointer();
  m_base_buffer_pointer = m_vertex_stream_buffer.GetHostPointer();
  m_end_buffer_pointer = vertices + MAXVBUFFERSIZE;
  m_cur_buffer_pointer = vertices;
  m_index_generator.Start(reinterpret_cast<u16*>(m_index_stream_buffer.GetCurrentHostPointer()));
}

void VertexManager::CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices,
                                 u32* out_base_vertex, u32* out_base_index)
{
  const u32 vertex_bytes = num_vertices * vertex_stride;
  const u32 index_bytes = num_indices * static_cast<u32>(sizeof(u16));

  // Draws index from the start of each buffer, so the bases are the current
  // offsets in elements.
  *out_base_vertex =
      vertex_stride != 0 ? m_vertex_stream_buffer.GetCurrentOffset() / vertex_stride : 0;
  *out_base_index = m_index_stream_buffer.GetCurrentOffset() / static_cast<u32>(sizeof(u16));

  m_vertex_stream_buffer.CommitMemory(vertex_bytes);
  m_index_stream_buffer.CommitMemory(index_bytes);
  ADDSTAT(g_stats.this_frame.bytes_vertex_streamed, static_cast<int>(vertex_bytes));
  ADDSTAT(g_stats.this_frame.bytes_index_streamed, static_cast<int>(index_bytes));

  Gfx* const gfx = Gfx::GetInstance();
  gfx->SetVertexBuffer(m_vertex_stream_buffer.GetGPUPointer(), m_vertex_srv.cpu_handle,
                       vertex_stride, m_vertex_stream_buffer.GetSize());
  gfx->SetIndexBuffer(m_index_stream_buffer.GetGPUPointer(), m_index_stream_buffer.GetSize(),
                      DXGI_FORMAT_R16_UINT);
}

void VertexManager::UploadUniforms()
{
  UpdateVertexShaderConstants();
  UpdateGeometryShaderConstants();
  UpdatePixelShaderConstants();
  UpdateCustomShaderConstants();
}

void VertexManager::UpdateVertexShaderConstants()
{
  auto& manager = Core::System::GetInstance().GetVertexShaderManager();
  if (!manager.dirty)
    return;
  if (!ReserveConstantStorage())
    return;

  StageBlock(m_uniform_stream_buffer, CB_VERTEX, &manager.constants, sizeof(manager.constants));
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, sizeof(VertexShaderConstants));
  manager.dirty = false;
}

void VertexManager::UpdateGeometryShaderConstants()
{
  auto& manager = Core::System::GetInstance().GetGeometryShaderManager();
  if (!manager.dirty)
    return;
  if (!ReserveConstantStorage())
    return;

  StageBlock(m_uniform_stream_buffer, CB_GEOMETRY, &manager.constants, sizeof(manager.constants));
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, sizeof(GeometryShaderConstants));
  manager.dirty = false;
}

void VertexManager::UpdatePixelShaderConstants()
{
  // Space is reserved before the dirty check here, so a full ring triggers
  // the full re-upload even when these constants are unchanged.
  auto& manager = Core::System::GetInstance().GetPixelShaderManager();
  if (!ReserveConstantStorage())
    return;
  if (!manager.dirty)
    return;

  StageBlock(m_uniform_stream_buffer, CB_PIXEL, &manager.constants, sizeof(manager.constants));
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, sizeof(PixelShaderConstants));
  manager.dirty = false;
}

void VertexManager::UpdateCustomShaderConstants()
{
  auto& manager = Core::System::GetInstance().GetPixelShaderManager();
  if (!ReserveConstantStorage())
    return;
  if (!manager.custom_constants_dirty)
    return;

  StageBlock(m_uniform_stream_buffer, CB_PIXEL_CUSTOM, manager.custom_constants.data(),
             static_cast<u32>(manager.custom_constants.size()));
  manager.custom_constants_dirty = false;
}

bool VertexManager::ReserveConstantStorage()
{
  auto& pixel_manager = Core::System::GetInstance().GetPixelShaderManager();
  const u32 wanted = LARGEST_BLOCK + static_cast<u32>(pixel_manager.custom_constants.size());
  if (m_uniform_stream_buffer.ReserveMemory(wanted, CB_ALIGN))
    return true;

  // Constant updates only happen where state can safely be rebuilt.
  WARN_LOG_FMT(VIDEO, "Executing command list while waiting for space in uniform buffer");
  Gfx::GetInstance()->ExecuteCommandList(false);

  // The new command list has no constants bound, so bind all of them now.
  UploadAllConstants();
  return false;
}

void VertexManager::UploadAllConstants()
{
  auto& system = Core::System::GetInstance();
  auto& pixel_manager = system.GetPixelShaderManager();
  auto& vertex_manager = system.GetVertexShaderManager();
  auto& geometry_manager = system.GetGeometryShaderManager();

  // Only reached right after a submission (or at start-up), so the space
  // should be there.
  const u32 custom_size = static_cast<u32>(pixel_manager.custom_constants.size());
  const u32 total = ALL_CUSTOM_AT + custom_size;
  if (!m_uniform_stream_buffer.ReserveMemory(total, CB_ALIGN))
  {
    PanicAlertFmt("Failed to allocate space for constants in streaming buffer");
    return;
  }

  const D3D12_GPU_VIRTUAL_ADDRESS gpu = m_uniform_stream_buffer.GetCurrentGPUPointer();
  Gfx* const gfx = Gfx::GetInstance();
  gfx->SetConstantBuffer(CB_PIXEL, gpu + ALL_PIXEL_AT);
  gfx->SetConstantBuffer(CB_VERTEX, gpu + ALL_VERTEX_AT);
  gfx->SetConstantBuffer(CB_PIXEL_CUSTOM, gpu + ALL_CUSTOM_AT);
  gfx->SetConstantBuffer(CB_GEOMETRY, gpu + ALL_GEOMETRY_AT);

  u8* const host = m_uniform_stream_buffer.GetCurrentHostPointer();
  std::memcpy(host + ALL_PIXEL_AT, &pixel_manager.constants, sizeof(PixelShaderConstants));
  std::memcpy(host + ALL_VERTEX_AT, &vertex_manager.constants, sizeof(VertexShaderConstants));
  std::memcpy(host + ALL_GEOMETRY_AT, &geometry_manager.constants, sizeof(GeometryShaderConstants));
  if (custom_size != 0)
    std::memcpy(host + ALL_CUSTOM_AT, pixel_manager.custom_constants.data(), custom_size);

  m_uniform_stream_buffer.CommitMemory(total);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, total);

  // The custom constants' flag is left alone, as it always has been.
  vertex_manager.dirty = false;
  geometry_manager.dirty = false;
  pixel_manager.dirty = false;
}

void VertexManager::UploadUtilityUniforms(const void* data, u32 data_size)
{
  InvalidateConstants();

  // After a submission the ring has room again; no second reservation is made.
  if (!m_uniform_stream_buffer.ReserveMemory(data_size, CB_ALIGN))
  {
    WARN_LOG_FMT(VIDEO, "Executing command buffer while waiting for ext space in uniform buffer");
    Gfx::GetInstance()->ExecuteCommandList(false);
  }

  // Utility shaders read the same block from the first three slots.
  const D3D12_GPU_VIRTUAL_ADDRESS gpu = m_uniform_stream_buffer.GetCurrentGPUPointer();
  Gfx* const gfx = Gfx::GetInstance();
  gfx->SetConstantBuffer(CB_PIXEL, gpu);
  gfx->SetConstantBuffer(CB_VERTEX, gpu);
  gfx->SetConstantBuffer(CB_PIXEL_CUSTOM, gpu);
  std::memcpy(m_uniform_stream_buffer.GetCurrentHostPointer(), data, data_size);
  m_uniform_stream_buffer.CommitMemory(data_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, data_size);
}

bool VertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                      u32* out_offset)
{
  if (data_size > m_texel_stream_buffer.GetSize())
    return false;

  const u32 element = GetTexelBufferElementSize(format);
  if (!m_texel_stream_buffer.ReserveMemory(data_size, element))
  {
    WARN_LOG_FMT(VIDEO, "Submitting command buffer while waiting for space in texel buffer");
    Gfx::GetInstance()->ExecuteCommandList(false);
    if (!m_texel_stream_buffer.ReserveMemory(data_size, element))
    {
      PanicAlertFmt("Failed to allocate {} bytes from texel buffer", data_size);
      return false;
    }
  }

  std::memcpy(m_texel_stream_buffer.GetCurrentHostPointer(), data, data_size);
  *out_offset = m_texel_stream_buffer.GetCurrentOffset() / element;
  m_texel_stream_buffer.CommitMemory(data_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, data_size);
  Gfx::GetInstance()->SetTextureDescriptor(0, m_texel_buffer_views[format].cpu_handle);
  return true;
}

bool VertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                      u32* out_offset, const void* palette_data, u32 palette_size,
                                      TexelBufferFormat palette_format, u32* out_palette_offset)
{
  const u32 element = GetTexelBufferElementSize(format);
  const u32 palette_element = GetTexelBufferElementSize(palette_format);

  // Room for aligning the palette after the data.
  const u32 wanted = data_size + palette_size + palette_element;
  if (wanted > m_texel_stream_buffer.GetSize())
    return false;

  if (!m_texel_stream_buffer.ReserveMemory(wanted, element))
  {
    WARN_LOG_FMT(VIDEO, "Submitting command buffer while waiting for space in texel buffer");
    Gfx::GetInstance()->ExecuteCommandList(false);
    if (!m_texel_stream_buffer.ReserveMemory(wanted, element))
    {
      PanicAlertFmt("Failed to allocate {} bytes from texel buffer", wanted);
      return false;
    }
  }

  const u32 palette_at = Common::AlignUp(data_size, palette_element);
  u8* const host = m_texel_stream_buffer.GetCurrentHostPointer();
  std::memcpy(host, data, data_size);
  std::memcpy(host + palette_at, palette_data, palette_size);

  const u32 base = m_texel_stream_buffer.GetCurrentOffset();
  *out_offset = base / element;
  *out_palette_offset = (base + palette_at) / palette_element;

  const u32 used = palette_at + palette_size;
  m_texel_stream_buffer.CommitMemory(used);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, used);
  Gfx* const gfx = Gfx::GetInstance();
  gfx->SetTextureDescriptor(0, m_texel_buffer_views[format].cpu_handle);
  gfx->SetTextureDescriptor(1, m_texel_buffer_views[palette_format].cpu_handle);
  return true;
}

}  // namespace DX12
