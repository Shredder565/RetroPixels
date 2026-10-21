// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D12/D3D12BoundingBox.h"

#include <cstring>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/D3D12/D3D12Gfx.h"
#include "VideoBackends/D3D12/DX12Context.h"

namespace DX12
{
namespace
{
D3D12_RESOURCE_DESC BBoxBufferDesc(u32 size, D3D12_RESOURCE_FLAGS flags)
{
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Alignment = 0;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = flags;
  return desc;
}

// The shader-side buffer sits in UNORDERED_ACCESS between copies.
void CopyBetween(ID3D12Resource* gpu_buffer, D3D12_RESOURCE_STATES copy_state, ID3D12Resource* dst,
                 u64 dst_offset, ID3D12Resource* src, u64 src_offset, u64 size)
{
  ID3D12GraphicsCommandList* const list = g_dx_context->GetCommandList();
  ResourceBarrier(list, gpu_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, copy_state);
  list->CopyBufferRegion(dst, dst_offset, src, src_offset, size);
  ResourceBarrier(list, gpu_buffer, copy_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
}  // namespace

D3D12BoundingBox::~D3D12BoundingBox()
{
  if (m_gpu_descriptor)
    g_dx_context->GetDescriptorHeapManager().Free(m_gpu_descriptor);
}

bool D3D12BoundingBox::Initialize()
{
  if (!CreateBuffers())
    return false;

  Gfx::GetInstance()->SetPixelShaderUAV(m_gpu_descriptor.cpu_handle);
  return true;
}

std::vector<BBoxType> D3D12BoundingBox::Read(u32 index, u32 length)
{
  // Bring the whole buffer back and wait for it.
  CopyBetween(m_gpu_buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, m_readback_buffer.Get(), 0,
              m_gpu_buffer.Get(), 0, BUFFER_SIZE);
  Gfx::GetInstance()->ExecuteCommandList(true);

  std::vector<BBoxType> result(length);
  const D3D12_RANGE whole = {0, BUFFER_SIZE};
  void* mapped;
  const HRESULT hr = m_readback_buffer->Map(0, &whole, &mapped);
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Map bounding box CPU buffer failed: {}", DX12HRWrap(hr));
  if (FAILED(hr))
    return result;

  const u8* const first = static_cast<const u8*>(mapped) + index * sizeof(BBoxType);
  std::memcpy(result.data(), first, length * sizeof(BBoxType));

  const D3D12_RANGE nothing_written = {0, 0};
  m_readback_buffer->Unmap(0, &nothing_written);
  return result;
}

void D3D12BoundingBox::Write(u32 index, std::span<const BBoxType> values)
{
  const u32 bytes = static_cast<u32>(values.size() * sizeof(BBoxType));
  if (!m_upload_buffer.ReserveMemory(bytes, sizeof(BBoxType)))
  {
    WARN_LOG_FMT(VIDEO, "Executing command list while waiting for space in bbox stream buffer");
    Gfx::GetInstance()->ExecuteCommandList(false);
    if (!m_upload_buffer.ReserveMemory(bytes, sizeof(BBoxType)))
    {
      PanicAlertFmt("Failed to allocate bbox stream buffer space");
      return;
    }
  }

  const u32 staged_at = m_upload_buffer.GetCurrentOffset();
  std::memcpy(m_upload_buffer.GetCurrentHostPointer(), values.data(), bytes);
  m_upload_buffer.CommitMemory(bytes);

  CopyBetween(m_gpu_buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, m_gpu_buffer.Get(),
              index * sizeof(BBoxType), m_upload_buffer.GetBuffer(), staged_at, bytes);
}

bool D3D12BoundingBox::CreateBuffers()
{
  ID3D12Device* const device = g_dx_context->GetDevice();

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  const D3D12_RESOURCE_DESC gpu_desc =
      BBoxBufferDesc(BUFFER_SIZE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &gpu_desc,
                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                               IID_PPV_ARGS(&m_gpu_buffer));
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Creating bounding box GPU buffer failed: {}", DX12HRWrap(hr));
  if (FAILED(hr))
    return false;
  if (!g_dx_context->GetDescriptorHeapManager().Allocate(&m_gpu_descriptor))
    return false;

  // One signed 32-bit element per value.
  D3D12_UNORDERED_ACCESS_VIEW_DESC view = {};
  view.Format = DXGI_FORMAT_R32_SINT;
  view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  view.Buffer.NumElements = NUM_BBOX_VALUES;
  device->CreateUnorderedAccessView(m_gpu_buffer.Get(), nullptr, &view,
                                    m_gpu_descriptor.cpu_handle);

  heap.Type = D3D12_HEAP_TYPE_READBACK;
  const D3D12_RESOURCE_DESC readback_desc = BBoxBufferDesc(BUFFER_SIZE, D3D12_RESOURCE_FLAG_NONE);
  hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                       IID_PPV_ARGS(&m_readback_buffer));
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Creating bounding box CPU buffer failed: {}", DX12HRWrap(hr));
  if (FAILED(hr))
    return false;

  return m_upload_buffer.AllocateBuffer(STREAM_BUFFER_SIZE);
}
}  // namespace DX12
