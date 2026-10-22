// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>

#include "VideoBackends/D3D12/DescriptorHeapManager.h"
#include "VideoCommon/Constants.h"

namespace DX12
{
// A shader-visible descriptor heap handed out front to back and emptied as a
// whole once the command list that used it has finished.
class DescriptorAllocator
{
public:
  DescriptorAllocator();
  ~DescriptorAllocator();

  ID3D12DescriptorHeap* GetDescriptorHeap() const { return m_descriptor_heap.Get(); }
  u32 GetDescriptorIncrementSize() const { return m_descriptor_increment_size; }

  bool Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 num_descriptors);

  // Contiguous run of num_handles descriptors; false when the heap is full.
  bool Allocate(u32 num_handles, DescriptorHandle* out_base_handle);
  void Reset();

protected:
  ComPtr<ID3D12DescriptorHeap> m_descriptor_heap;
  u32 m_descriptor_increment_size = 0;
  u32 m_num_descriptors = 0;
  u32 m_current_offset = 0;

  D3D12_CPU_DESCRIPTOR_HANDLE m_heap_base_cpu = {};
  D3D12_GPU_DESCRIPTOR_HANDLE m_heap_base_gpu = {};
};

// The sampler states of all pixel-stage slots, compared as raw bytes.
struct SamplerStateSet final
{
  SamplerState states[VideoCommon::MAX_PIXEL_SHADER_SAMPLERS];
};

bool operator==(const SamplerStateSet& lhs, const SamplerStateSet& rhs);
bool operator!=(const SamplerStateSet& lhs, const SamplerStateSet& rhs);
bool operator<(const SamplerStateSet& lhs, const SamplerStateSet& rhs);

// Shader-visible sampler heap holding whole sets of pixel-stage samplers;
// a set already copied in is reused.
class SamplerAllocator final : public DescriptorAllocator
{
public:
  SamplerAllocator();
  ~SamplerAllocator();

  bool Create(ID3D12Device* device);
  bool GetGroupHandle(const SamplerStateSet& sss, D3D12_GPU_DESCRIPTOR_HANDLE* handle);
  bool ShouldReset() const;
  void Reset();

private:
  std::map<SamplerStateSet, D3D12_GPU_DESCRIPTOR_HANDLE> m_sampler_map;
};

}  // namespace DX12
