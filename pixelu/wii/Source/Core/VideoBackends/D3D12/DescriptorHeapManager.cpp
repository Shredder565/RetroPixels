// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D12/DescriptorHeapManager.h"

#include <array>

#include "Common/Assert.h"

#include "VideoBackends/D3D12/DX12Context.h"
#include "VideoCommon/VideoConfig.h"

namespace DX12
{
DescriptorHeapManager::DescriptorHeapManager() = default;
DescriptorHeapManager::~DescriptorHeapManager() = default;

bool DescriptorHeapManager::Create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                   u32 num_descriptors)
{
  D3D12_DESCRIPTOR_HEAP_DESC desc = {};
  desc.Type = type;
  desc.NumDescriptors = num_descriptors;
  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

  const HRESULT result = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_descriptor_heap));
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create descriptor heap: {}", DX12HRWrap(result));
  if (FAILED(result))
    return false;

  m_heap_base_cpu = m_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
  m_num_descriptors = num_descriptors;
  m_descriptor_increment_size = device->GetDescriptorHandleIncrementSize(type);

  // Every block starts all free. The last block is whole even when the heap
  // ends part-way through it.
  const u32 blocks = (num_descriptors + BITSET_SIZE - 1) / BITSET_SIZE;
  m_free_slots.assign(blocks, BitSetType().set());
  return true;
}

bool DescriptorHeapManager::Allocate(DescriptorHandle* handle)
{
  for (u32 block = 0; block < m_free_slots.size(); ++block)
  {
    BitSetType& free_bits = m_free_slots[block];
    if (free_bits.none())
      continue;

    u32 bit = 0;
    while (!free_bits.test(bit))
      ++bit;
    free_bits.reset(bit);

    const u32 slot = block * BITSET_SIZE + bit;
    handle->index = slot;
    handle->cpu_handle.ptr = m_heap_base_cpu.ptr + slot * m_descriptor_increment_size;
    handle->gpu_handle.ptr = 0;
    return true;
  }

  PanicAlertFmt("Out of fixed descriptors");
  return false;
}

void DescriptorHeapManager::Free(u32 index)
{
  ASSERT(index < m_num_descriptors);
  m_free_slots[index / BITSET_SIZE].set(index % BITSET_SIZE);
}

void DescriptorHeapManager::Free(const DescriptorHandle& handle)
{
  Free(handle.index);
}

SamplerHeapManager::SamplerHeapManager() = default;
SamplerHeapManager::~SamplerHeapManager() = default;

namespace
{
// Filter for (min, mag, mip) linear bits: index = min*4 + mag*2 + mip.
constexpr std::array<D3D12_FILTER, 8> SAMPLER_FILTERS = {
    D3D12_FILTER_MIN_MAG_MIP_POINT,
    D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR,
    D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT,
    D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR,
    D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT,
    D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
    D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
    D3D12_FILTER_MIN_MAG_MIP_LINEAR,
};

// By WrapMode; the invalid value 3 acts as clamp, as on hardware.
constexpr std::array<D3D12_TEXTURE_ADDRESS_MODE, 4> SAMPLER_ADDRESS_MODES = {
    D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
    D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP};

D3D12_SAMPLER_DESC SamplerDescFor(const SamplerState& state)
{
  const u32 min_bit = state.tm0.min_filter == FilterMode::Linear ? 1 : 0;
  const u32 mag_bit = state.tm0.mag_filter == FilterMode::Linear ? 1 : 0;
  const u32 mip_bit = state.tm0.mipmap_filter == FilterMode::Linear ? 1 : 0;

  // Border colour stays zero; anisotropy stays zero unless anisotropic.
  D3D12_SAMPLER_DESC desc = {};
  desc.Filter = SAMPLER_FILTERS[min_bit * 4 + mag_bit * 2 + mip_bit];
  desc.AddressU = SAMPLER_ADDRESS_MODES[static_cast<u32>(state.tm0.wrap_u.Value())];
  desc.AddressV = SAMPLER_ADDRESS_MODES[static_cast<u32>(state.tm0.wrap_v.Value())];
  desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  // LODs in 1/16 steps, bias in 1/256 steps.
  desc.MaxLOD = static_cast<float>(state.tm1.max_lod.Value()) / 16.f;
  desc.MinLOD = static_cast<float>(state.tm1.min_lod.Value()) / 16.f;
  desc.MipLODBias = static_cast<float>(static_cast<s32>(state.tm0.lod_bias)) / 256.f;
  desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;

  if (const u32 aniso_log2 = state.tm0.anisotropic_filtering; aniso_log2 != 0)
  {
    desc.Filter = D3D12_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = 1u << aniso_log2;
  }
  return desc;
}
}  // namespace

bool SamplerHeapManager::Lookup(const SamplerState& ss, D3D12_CPU_DESCRIPTOR_HANDLE* handle)
{
  if (const auto found = m_sampler_map.find(ss); found != m_sampler_map.end())
  {
    *handle = found->second;
    return true;
  }

  // Starting over is safe at any point: descriptors are copied to the
  // shader-visible heap when used. It costs recreating the samplers.
  if (m_current_offset == m_num_descriptors)
  {
    WARN_LOG_FMT(VIDEO, "Out of samplers, resetting CPU heap");
    Clear();
  }

  const D3D12_SAMPLER_DESC desc = SamplerDescFor(ss);
  const D3D12_CPU_DESCRIPTOR_HANDLE slot = {m_heap_base_cpu.ptr +
                                            m_current_offset * m_descriptor_increment_size};
  g_dx_context->GetDevice()->CreateSampler(&desc, slot);

  m_sampler_map.emplace(ss, slot);
  ++m_current_offset;
  *handle = slot;
  return true;
}

void SamplerHeapManager::Clear()
{
  m_sampler_map.clear();
  m_current_offset = 0;
}

bool SamplerHeapManager::Create(ID3D12Device* device, u32 num_descriptors)
{
  D3D12_DESCRIPTOR_HEAP_DESC desc = {};
  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  desc.NumDescriptors = num_descriptors;

  const HRESULT result = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_descriptor_heap));
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create sampler descriptor heap: {}",
             DX12HRWrap(result));
  if (FAILED(result))
    return false;

  m_num_descriptors = num_descriptors;
  m_descriptor_increment_size =
      device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  m_heap_base_cpu = m_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
  return true;
}
}  // namespace DX12
