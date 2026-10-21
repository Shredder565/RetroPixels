// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D12/DescriptorAllocator.h"

#include <array>
#include <cstring>

#include "Common/Assert.h"

#include "VideoBackends/D3D12/DX12Context.h"

namespace DX12
{
DescriptorAllocator::DescriptorAllocator() = default;
DescriptorAllocator::~DescriptorAllocator() = default;

bool DescriptorAllocator::Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                 u32 num_descriptors)
{
  D3D12_DESCRIPTOR_HEAP_DESC desc = {};
  desc.Type = type;
  desc.NumDescriptors = num_descriptors;
  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

  const HRESULT result = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_descriptor_heap));
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Creating descriptor heap for linear allocator failed: {}",
             DX12HRWrap(result));
  if (FAILED(result))
    return false;

  m_num_descriptors = num_descriptors;
  m_descriptor_increment_size = device->GetDescriptorHandleIncrementSize(type);
  m_heap_base_cpu = m_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
  m_heap_base_gpu = m_descriptor_heap->GetGPUDescriptorHandleForHeapStart();
  return true;
}

bool DescriptorAllocator::Allocate(u32 num_handles, DescriptorHandle* out_base_handle)
{
  if (m_current_offset + num_handles > m_num_descriptors)
    return false;

  const u32 byte_offset = m_current_offset * m_descriptor_increment_size;
  out_base_handle->index = m_current_offset;
  out_base_handle->cpu_handle.ptr = m_heap_base_cpu.ptr + byte_offset;
  out_base_handle->gpu_handle.ptr = m_heap_base_gpu.ptr + byte_offset;
  m_current_offset += num_handles;
  return true;
}

void DescriptorAllocator::Reset()
{
  m_current_offset = 0;
}

namespace
{
// SamplerState has no padding, so the sets order and compare as raw bytes.
int CompareSets(const SamplerStateSet& lhs, const SamplerStateSet& rhs)
{
  return std::memcmp(lhs.states, rhs.states, sizeof(lhs.states));
}
}  // namespace

bool operator==(const SamplerStateSet& lhs, const SamplerStateSet& rhs)
{
  return CompareSets(lhs, rhs) == 0;
}

bool operator!=(const SamplerStateSet& lhs, const SamplerStateSet& rhs)
{
  return CompareSets(lhs, rhs) != 0;
}

bool operator<(const SamplerStateSet& lhs, const SamplerStateSet& rhs)
{
  return CompareSets(lhs, rhs) < 0;
}

SamplerAllocator::SamplerAllocator() = default;
SamplerAllocator::~SamplerAllocator() = default;

bool SamplerAllocator::Create(ID3D12Device* device)
{
  return DescriptorAllocator::Create(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                     D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE);
}

bool SamplerAllocator::GetGroupHandle(const SamplerStateSet& sss,
                                      D3D12_GPU_DESCRIPTOR_HANDLE* handle)
{
  if (const auto found = m_sampler_map.find(sss); found != m_sampler_map.end())
  {
    *handle = found->second;
    return true;
  }

  constexpr u32 SLOTS = VideoCommon::MAX_PIXEL_SHADER_SAMPLERS;
  DescriptorHandle group;
  if (!Allocate(SLOTS, &group))
    return false;

  // Each state's CPU descriptor comes from the shared sampler heap.
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, SLOTS> sources;
  SamplerHeapManager& cache = g_dx_context->GetSamplerHeapManager();
  for (u32 slot = 0; slot < SLOTS; ++slot)
  {
    if (!cache.Lookup(sss.states[slot], &sources[slot]))
      return false;
  }

  // One destination range of all slots, gathered from single-descriptor sources.
  static constexpr std::array<UINT, SLOTS> ONE_EACH = {1, 1, 1, 1, 1, 1, 1, 1,
                                                       1, 1, 1, 1, 1, 1, 1, 1};
  g_dx_context->GetDevice()->CopyDescriptors(
      1, &group.cpu_handle, &VideoCommon::MAX_PIXEL_SHADER_SAMPLERS, SLOTS, sources.data(),
      ONE_EACH.data(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  *handle = group.gpu_handle;
  m_sampler_map.emplace(sss, group.gpu_handle);
  return true;
}

bool SamplerAllocator::ShouldReset() const
{
  // Starting over costs copying every set again, so the heap is only reset
  // once half of it would be in use.
  return m_sampler_map.size() >= (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE / 2);
}

void SamplerAllocator::Reset()
{
  DescriptorAllocator::Reset();
  m_sampler_map.clear();
}
}  // namespace DX12
