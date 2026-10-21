// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/VKVertexManager.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "Common/Align.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "Core/System.h"

#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKGfx.h"
#include "VideoBackends/Vulkan/VKStreamBuffer.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexShaderManager.h"

namespace Vulkan
{
namespace
{
constexpr u32 INDEX_BYTES = sizeof(u16);
constexpr u32 PS_SIZE = sizeof(PixelShaderConstants);
constexpr u32 VS_SIZE = sizeof(VertexShaderConstants);
constexpr u32 GS_SIZE = sizeof(GeometryShaderConstants);

StateTracker* Tracker()
{
  return StateTracker::GetInstance();
}

u32 UniformAlignment()
{
  return static_cast<u32>(g_vulkan_context->GetUniformBufferAlignment());
}

void SubmitWithWarning(const char* what)
{
  WARN_LOG_FMT(VIDEO, "{}", what);
  VKGfx::GetInstance()->ExecuteCommandBuffer(false);
}

// A view over the whole texel ring; draws address it with element offsets.
VkBufferView WholeBufferView(VkBuffer buffer, VkFormat format)
{
  VkBufferViewCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
  info.buffer = buffer;
  info.format = format;
  info.offset = 0;
  info.range = VK_WHOLE_SIZE;

  VkBufferView view = VK_NULL_HANDLE;
  const VkResult res = vkCreateBufferView(g_vulkan_context->GetDevice(), &info, nullptr, &view);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateBufferView failed: ");
    return VK_NULL_HANDLE;
  }
  return view;
}

VkFormat TexelViewFormat(u32 format)
{
  switch (format)
  {
  case TEXEL_BUFFER_FORMAT_R8_UINT:
    return VK_FORMAT_R8_UINT;
  case TEXEL_BUFFER_FORMAT_R16_UINT:
    return VK_FORMAT_R16_UINT;
  case TEXEL_BUFFER_FORMAT_RGBA8_UINT:
    return VK_FORMAT_R8G8B8A8_UINT;
  default:
    return VK_FORMAT_R32G32_UINT;
  }
}
}  // namespace

VertexManager::VertexManager() = default;

VertexManager::~VertexManager()
{
  for (VkBufferView view : m_texel_views)
  {
    if (view != VK_NULL_HANDLE)
      vkDestroyBufferView(g_vulkan_context->GetDevice(), view, nullptr);
  }
}

bool VertexManager::Initialize()
{
  if (!VertexManagerBase::Initialize())
    return false;

  // The vertex ring is also read as a storage buffer by the dynamic vertex loader.
  m_vertex_stream_buffer =
      StreamBuffer::Create(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VERTEX_STREAM_BUFFER_SIZE);
  m_index_stream_buffer =
      StreamBuffer::Create(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, INDEX_STREAM_BUFFER_SIZE);
  m_uniform_stream_buffer =
      StreamBuffer::Create(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, UNIFORM_STREAM_BUFFER_SIZE);
  if (!m_vertex_stream_buffer || !m_index_stream_buffer || !m_uniform_stream_buffer)
  {
    PanicAlertFmt("Failed to allocate streaming buffers");
    return false;
  }

  // Always reserve room for all three blocks, even when fewer are written: validation
  // complains when the largest offset plus the largest range passes the end of the buffer.
  const u32 alignment = UniformAlignment();
  m_constants_reserve = Common::AlignUp(PS_SIZE, alignment) + VS_SIZE;
  m_constants_reserve = Common::AlignUp(m_constants_reserve, alignment) + GS_SIZE;

  // Up to 16 MiB, bounded by the device's texel buffer limit (elements may be single bytes).
  const u32 texel_size =
      std::min(TEXEL_STREAM_BUFFER_SIZE, g_vulkan_context->GetDeviceInfo().maxTexelBufferElements);
  m_texel_stream_buffer = StreamBuffer::Create(VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, texel_size);
  if (!m_texel_stream_buffer)
  {
    PanicAlertFmt("Failed to allocate streaming texel buffer");
    return false;
  }

  for (u32 format = 0; format < NUM_TEXEL_BUFFER_FORMATS; ++format)
  {
    m_texel_views[format] =
        WholeBufferView(m_texel_stream_buffer->GetBuffer(), TexelViewFormat(format));
    if (m_texel_views[format] == VK_NULL_HANDLE)
    {
      PanicAlertFmt("Failed to create texel buffer view");
      return false;
    }
  }

  // Give every binding something valid from the start, used or not.
  UploadAllConstants();
  Tracker()->SetUtilityUniformBuffer(m_uniform_stream_buffer->GetBuffer(), 0, VS_SIZE);
  for (u32 slot = 0; slot < NUM_COMPUTE_TEXEL_BUFFERS; ++slot)
    Tracker()->SetTexelBuffer(slot, m_texel_views[TEXEL_BUFFER_FORMAT_R8_UINT]);
  return true;
}

void VertexManager::ResetBuffer(u32 vertex_stride)
{
  const u32 index_bytes = MAXIBUFFERSIZE * INDEX_BYTES;
  bool have_vertices = m_vertex_stream_buffer->ReserveMemory(MAXVBUFFERSIZE, vertex_stride);
  bool have_indices = m_index_stream_buffer->ReserveMemory(index_bytes, INDEX_BYTES);
  if (!have_vertices || !have_indices)
  {
    // Submitting lets the rings wait on fences for space.
    SubmitWithWarning("Executing command list while waiting for space in vertex/index buffer");
    if (!have_vertices)
      have_vertices = m_vertex_stream_buffer->ReserveMemory(MAXVBUFFERSIZE, vertex_stride);
    if (!have_indices)
      have_indices = m_index_stream_buffer->ReserveMemory(index_bytes, INDEX_BYTES);
    if (!have_vertices || !have_indices)
      PanicAlertFmt("Failed to allocate space in streaming buffers for pending draw");
  }

  m_base_buffer_pointer = m_vertex_stream_buffer->GetHostPointer();
  m_end_buffer_pointer = m_vertex_stream_buffer->GetCurrentHostPointer() + MAXVBUFFERSIZE;
  m_cur_buffer_pointer = m_vertex_stream_buffer->GetCurrentHostPointer();
  m_index_generator.Start(reinterpret_cast<u16*>(m_index_stream_buffer->GetCurrentHostPointer()));
}

void VertexManager::CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices,
                                 u32* out_base_vertex, u32* out_base_index)
{
  const u32 vertex_bytes = num_vertices * vertex_stride;
  const u32 index_bytes = num_indices * INDEX_BYTES;

  // Draws use base vertex/index offsets into buffers bound at offset 0.
  *out_base_vertex = vertex_stride != 0 ? m_vertex_stream_buffer->GetCurrentOffset() / vertex_stride : 0;
  *out_base_index = m_index_stream_buffer->GetCurrentOffset() / INDEX_BYTES;

  m_vertex_stream_buffer->CommitMemory(vertex_bytes);
  m_index_stream_buffer->CommitMemory(index_bytes);
  ADDSTAT(g_stats.this_frame.bytes_vertex_streamed, static_cast<int>(vertex_bytes));
  ADDSTAT(g_stats.this_frame.bytes_index_streamed, static_cast<int>(index_bytes));

  Tracker()->SetVertexBuffer(m_vertex_stream_buffer->GetBuffer(), 0, VERTEX_STREAM_BUFFER_SIZE);
  Tracker()->SetIndexBuffer(m_index_stream_buffer->GetBuffer(), 0, VK_INDEX_TYPE_UINT16);
}

void VertexManager::StreamConstants(u32 binding, const void* data, u32 size)
{
  Tracker()->SetGXUniformBuffer(binding, m_uniform_stream_buffer->GetBuffer(),
                                m_uniform_stream_buffer->GetCurrentOffset(), size);
  std::memcpy(m_uniform_stream_buffer->GetCurrentHostPointer(), data, size);
  m_uniform_stream_buffer->CommitMemory(size);
}

void VertexManager::UploadUniforms()
{
  auto& system = Core::System::GetInstance();

  // Each check reserves the full constant area again; if that needed a submission everything
  // was just uploaded and the stage is done.
  auto& vs = system.GetVertexShaderManager();
  if (vs.dirty && ReserveConstantStorage())
  {
    StreamConstants(UBO_DESCRIPTOR_SET_BINDING_VS, &vs.constants, VS_SIZE);
    ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, static_cast<int>(VS_SIZE));
    vs.dirty = false;
  }

  auto& gs = system.GetGeometryShaderManager();
  if (gs.dirty && ReserveConstantStorage())
  {
    StreamConstants(UBO_DESCRIPTOR_SET_BINDING_GS, &gs.constants, GS_SIZE);
    ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, static_cast<int>(GS_SIZE));
    gs.dirty = false;
  }

  // The pixel stage reserves even when nothing is dirty.
  auto& ps = system.GetPixelShaderManager();
  if (!ReserveConstantStorage())
    return;
  if (ps.dirty)
  {
    StreamConstants(UBO_DESCRIPTOR_SET_BINDING_PS, &ps.constants, PS_SIZE);
    ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, static_cast<int>(PS_SIZE));
    ps.dirty = false;
  }
  if (ps.custom_constants_dirty)
  {
    StreamConstants(UBO_DESCRIPTOR_SET_BINDING_CUST, ps.custom_constants.data(),
                    static_cast<u32>(ps.custom_constants.size()));
    ps.custom_constants_dirty = false;
  }
}

bool VertexManager::ReserveConstantStorage()
{
  auto& ps = Core::System::GetInstance().GetPixelShaderManager();
  const u32 custom_size = static_cast<u32>(ps.custom_constants.size());
  if (m_uniform_stream_buffer->ReserveMemory(m_constants_reserve + custom_size, UniformAlignment()))
    return true;

  // Callers can cope with state being re-recorded. The new command buffer has none of the
  // old constants, so upload all of them now.
  SubmitWithWarning("Executing command buffer while waiting for space in uniform buffer");
  UploadAllConstants();
  return false;
}

void VertexManager::UploadAllConstants()
{
  auto& system = Core::System::GetInstance();
  auto& ps = system.GetPixelShaderManager();
  const u32 custom_size = static_cast<u32>(ps.custom_constants.size());

  // One allocation: PS, VS, GS, then the custom block, each aligned.
  const u32 alignment = UniformAlignment();
  const u32 ps_at = 0;
  const u32 vs_at = Common::AlignUp(ps_at + PS_SIZE, alignment);
  const u32 gs_at = Common::AlignUp(vs_at + VS_SIZE, alignment);
  const u32 custom_at = Common::AlignUp(gs_at + GS_SIZE, alignment);
  const u32 total = custom_at + custom_size;

  if (!m_uniform_stream_buffer->ReserveMemory(total, alignment))
  {
    PanicAlertFmt("Failed to allocate space for constants in streaming buffer");
    return;
  }

  auto& vs = system.GetVertexShaderManager();
  auto& gs = system.GetGeometryShaderManager();
  const VkBuffer buffer = m_uniform_stream_buffer->GetBuffer();
  const u32 base = m_uniform_stream_buffer->GetCurrentOffset();
  u8* const dest = m_uniform_stream_buffer->GetCurrentHostPointer();

  Tracker()->SetGXUniformBuffer(UBO_DESCRIPTOR_SET_BINDING_PS, buffer, base + ps_at, PS_SIZE);
  Tracker()->SetGXUniformBuffer(UBO_DESCRIPTOR_SET_BINDING_VS, buffer, base + vs_at, VS_SIZE);
  if (custom_size != 0)
  {
    Tracker()->SetGXUniformBuffer(UBO_DESCRIPTOR_SET_BINDING_CUST, buffer, base + custom_at,
                                  custom_size);
  }
  Tracker()->SetGXUniformBuffer(UBO_DESCRIPTOR_SET_BINDING_GS, buffer, base + gs_at, GS_SIZE);

  std::memcpy(dest + ps_at, &ps.constants, PS_SIZE);
  std::memcpy(dest + vs_at, &vs.constants, VS_SIZE);
  std::memcpy(dest + gs_at, &gs.constants, GS_SIZE);
  if (custom_size != 0)
    std::memcpy(dest + custom_at, ps.custom_constants.data(), custom_size);

  m_uniform_stream_buffer->CommitMemory(total);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, static_cast<int>(total));

  // The custom block's dirty flag is left alone, as before.
  vs.dirty = false;
  gs.dirty = false;
  ps.dirty = false;
}

void VertexManager::UploadUtilityUniforms(const void* data, u32 data_size)
{
  // GX constants must be bound again after a utility draw.
  InvalidateConstants();
  if (!m_uniform_stream_buffer->ReserveMemory(data_size, UniformAlignment()))
    SubmitWithWarning("Executing command buffer while waiting for ext space in uniform buffer");

  Tracker()->SetUtilityUniformBuffer(m_uniform_stream_buffer->GetBuffer(),
                                     m_uniform_stream_buffer->GetCurrentOffset(), data_size);
  std::memcpy(m_uniform_stream_buffer->GetCurrentHostPointer(), data, data_size);
  m_uniform_stream_buffer->CommitMemory(data_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, static_cast<int>(data_size));
}

bool VertexManager::ReserveTexelSpace(u32 size, u32 alignment)
{
  if (m_texel_stream_buffer->ReserveMemory(size, alignment))
    return true;
  WARN_LOG_FMT(VIDEO, "Submitting command buffer while waiting for space in texel buffer");
  VKGfx::GetInstance()->ExecuteCommandBuffer(false, false);
  if (m_texel_stream_buffer->ReserveMemory(size, alignment))
    return true;
  PanicAlertFmt("Failed to allocate {} bytes from texel buffer", size);
  return false;
}

bool VertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                      u32* out_offset)
{
  if (data_size > m_texel_stream_buffer->GetCurrentSize())
    return false;

  const u32 elem_size = GetTexelBufferElementSize(format);
  if (!ReserveTexelSpace(data_size, elem_size))
    return false;

  std::memcpy(m_texel_stream_buffer->GetCurrentHostPointer(), data, data_size);
  *out_offset = m_texel_stream_buffer->GetCurrentOffset() / elem_size;
  m_texel_stream_buffer->CommitMemory(data_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, static_cast<int>(data_size));
  Tracker()->SetTexelBuffer(0, m_texel_views[format]);
  return true;
}

bool VertexManager::UploadTexelBuffer(const void* data, u32 data_size, TexelBufferFormat format,
                                      u32* out_offset, const void* palette_data, u32 palette_size,
                                      TexelBufferFormat palette_format, u32* out_palette_offset)
{
  const u32 elem_size = GetTexelBufferElementSize(format);
  const u32 palette_elem_size = GetTexelBufferElementSize(palette_format);
  // Room for the palette's alignment padding as well.
  const u32 reserve = data_size + palette_size + palette_elem_size;
  if (reserve > m_texel_stream_buffer->GetCurrentSize())
    return false;
  if (!ReserveTexelSpace(reserve, elem_size))
    return false;

  // Data first, then the palette at the next palette-element boundary.
  const u32 palette_at = Common::AlignUp(data_size, palette_elem_size);
  u8* const dest = m_texel_stream_buffer->GetCurrentHostPointer();
  std::memcpy(dest, data, data_size);
  std::memcpy(dest + palette_at, palette_data, palette_size);

  const u32 base = m_texel_stream_buffer->GetCurrentOffset();
  *out_offset = base / elem_size;
  *out_palette_offset = (base + palette_at) / palette_elem_size;

  m_texel_stream_buffer->CommitMemory(palette_at + palette_size);
  ADDSTAT(g_stats.this_frame.bytes_uniform_streamed, static_cast<int>(palette_at + palette_size));
  Tracker()->SetTexelBuffer(0, m_texel_views[format]);
  Tracker()->SetTexelBuffer(1, m_texel_views[palette_format]);
  return true;
}
}  // namespace Vulkan
