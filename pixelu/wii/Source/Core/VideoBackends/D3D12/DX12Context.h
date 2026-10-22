// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/HRWrap.h"

#include "VideoBackends/D3D12/Common.h"
#include "VideoBackends/D3D12/D3D12StreamBuffer.h"
#include "VideoBackends/D3D12/DescriptorAllocator.h"
#include "VideoBackends/D3D12/DescriptorHeapManager.h"

struct IDXGIFactory;

namespace DX12
{
// Root parameter slots of the GX and utility root signatures.
enum ROOT_PARAMETER
{
  ROOT_PARAMETER_PS_CBV,
  ROOT_PARAMETER_PS_SRV,
  ROOT_PARAMETER_PS_SAMPLERS,
  ROOT_PARAMETER_VS_CBV,
  ROOT_PARAMETER_VS_CBV2,
  ROOT_PARAMETER_VS_CUS_CBV,
  ROOT_PARAMETER_GS_CBV,
  ROOT_PARAMETER_VS_SRV,
  ROOT_PARAMETER_BASE_VERTEX_CONSTANT,
  ROOT_PARAMETER_PS_CUS_CBV,
  ROOT_PARAMETER_PS_UAV_OR_CBV2,
  ROOT_PARAMETER_PS_CBV2,  // takes ROOT_PARAMETER_PS_UAV_OR_CBV2's slot without bounding box
  NUM_ROOT_PARAMETERS
};
// Root parameter slots of the compute root signature.
enum CS_ROOT_PARAMETERS
{
  CS_ROOT_PARAMETER_CBV,
  CS_ROOT_PARAMETER_SRV,
  CS_ROOT_PARAMETER_SAMPLERS,
  CS_ROOT_PARAMETER_UAV,
  NUM_CS_ROOT_PARAMETERS,
};

// The D3D12 device and everything shared by the backend: the queue and its
// fence, a ring of command lists with their per-list descriptor heaps and
// deferred releases, the descriptor heaps, the root signatures and the
// texture upload buffer.
class DXContext
{
public:
  ~DXContext();

  // Sample counts the EFB colour format supports (with a throwaway device
  // when the backend is not running).
  static std::vector<u32> GetAAModes(u32 adapter_index);

  // Loads the runtime and creates the factory, device, queue and fence.
  static bool Create(u32 adapter_index, bool enable_debug_layer);

  static void Destroy();

  IDXGIFactory* GetDXGIFactory() const { return m_dxgi_factory.Get(); }
  ID3D12Device* GetDevice() const { return m_device.Get(); }
  ID3D12CommandQueue* GetCommandQueue() const { return m_command_queue.Get(); }

  // The command list being recorded.
  ID3D12GraphicsCommandList* GetCommandList() const
  {
    return m_command_lists[m_current_command_list].command_list.Get();
  }
  DescriptorAllocator* GetDescriptorAllocator()
  {
    return &m_command_lists[m_current_command_list].descriptor_allocator;
  }
  SamplerAllocator* GetSamplerAllocator()
  {
    return &m_command_lists[m_current_command_list].sampler_allocator;
  }

  DescriptorHeapManager& GetDescriptorHeapManager() { return m_descriptor_heap_manager; }
  DescriptorHeapManager& GetRTVHeapManager() { return m_rtv_heap_manager; }
  DescriptorHeapManager& GetDSVHeapManager() { return m_dsv_heap_manager; }
  SamplerHeapManager& GetSamplerHeapManager() { return m_sampler_heap_manager; }
  // The shader-visible heaps of the current list: views, then samplers.
  ID3D12DescriptorHeap* const* GetGPUDescriptorHeaps() const
  {
    return m_gpu_descriptor_heaps.data();
  }
  u32 GetGPUDescriptorHeapCount() const { return static_cast<u32>(m_gpu_descriptor_heaps.size()); }
  const DescriptorHandle& GetNullSRVDescriptor() const { return m_null_srv_descriptor; }

  ID3D12RootSignature* GetGXRootSignature() const { return m_gx_root_signature.Get(); }
  ID3D12RootSignature* GetUtilityRootSignature() const { return m_utility_root_signature.Get(); }
  ID3D12RootSignature* GetComputeRootSignature() const { return m_compute_root_signature.Get(); }

  // Fence value the current command list will signal.
  u64 GetCurrentFenceValue() const { return m_current_fence_value; }

  // Highest fence value known to have completed.
  u64 GetCompletedFenceValue() const { return m_completed_fence_value; }

  StreamBuffer& GetTextureUploadBuffer() { return m_texture_upload_buffer; }

  D3D_FEATURE_LEVEL GetFeatureLevel() const { return m_feature_level; }

  // Whether the format can be a sampled 2D and cube texture.
  bool SupportsTextureFormat(DXGI_FORMAT format);

  // Descriptor heaps, root signatures, upload buffer and command lists.
  bool CreateGlobalResources();

  // Submits the current command list and starts the next one.
  void ExecuteCommandList(bool wait_for_completion);

  void WaitForFence(u64 fence);

  // Keeps a reference until the current command list has completed.
  void DeferResourceDestruction(ID3D12Resource* resource);

  // Frees the descriptor once the current command list has completed.
  void DeferDescriptorDestruction(DescriptorHeapManager& manager, u32 index);

  void ResetSamplerAllocators();

  // Rebuilds the GX root signature after a host-config change (bounding box,
  // per-pixel lighting, line/point expansion).
  void RecreateGXRootSignature();

private:
  // Lists in flight: one recording while the others execute.
  static const u32 NUM_COMMAND_LISTS = 3;

  // Larger texture uploads go through their own staging buffer.
  static const u32 TEXTURE_UPLOAD_BUFFER_SIZE = 32 * 1024 * 1024;

  struct CommandListResources
  {
    ComPtr<ID3D12CommandAllocator> command_allocator;
    ComPtr<ID3D12GraphicsCommandList> command_list;
    DescriptorAllocator descriptor_allocator;
    SamplerAllocator sampler_allocator;
    std::vector<ID3D12Resource*> pending_resources;
    std::vector<std::pair<DescriptorHeapManager&, u32>> pending_descriptors;
    u64 ready_fence_value = 0;
  };

  DXContext();

  bool CreateDXGIFactory(bool enable_debug_layer);
  bool CreateDevice(u32 adapter_index, bool enable_debug_layer);
  bool CreateCommandQueue();
  bool CreateFence();
  bool CreateDescriptorHeaps();
  bool CreateRootSignatures();
  bool CreateGXRootSignature();
  bool CreateUtilityRootSignature();
  bool CreateComputeRootSignature();
  bool CreateTextureUploadBuffer();
  bool CreateCommandLists();
  void MoveToNextCommandList();
  void DestroyPendingResources(CommandListResources& cmdlist);

  // Member order is relied on for the release order at shutdown.
  ComPtr<IDXGIFactory> m_dxgi_factory;
  ComPtr<ID3D12Debug> m_debug_interface;
  ComPtr<ID3D12Device> m_device;
  ComPtr<ID3D12CommandQueue> m_command_queue;

  ComPtr<ID3D12Fence> m_fence = nullptr;
  HANDLE m_fence_event = {};
  u32 m_current_fence_value = 0;
  u64 m_completed_fence_value = 0;

  std::array<CommandListResources, NUM_COMMAND_LISTS> m_command_lists;
  u32 m_current_command_list = NUM_COMMAND_LISTS - 1;

  DescriptorHeapManager m_descriptor_heap_manager;
  DescriptorHeapManager m_rtv_heap_manager;
  DescriptorHeapManager m_dsv_heap_manager;
  SamplerHeapManager m_sampler_heap_manager;
  std::array<ID3D12DescriptorHeap*, 2> m_gpu_descriptor_heaps = {};
  DescriptorHandle m_null_srv_descriptor;
  D3D_FEATURE_LEVEL m_feature_level = D3D_FEATURE_LEVEL_11_0;

  ComPtr<ID3D12RootSignature> m_gx_root_signature;
  ComPtr<ID3D12RootSignature> m_utility_root_signature;
  ComPtr<ID3D12RootSignature> m_compute_root_signature;

  StreamBuffer m_texture_upload_buffer;
};

extern std::unique_ptr<DXContext> g_dx_context;

// fmt wrapper for an HRESULT from D3D12 (HRESULT is a plain typedef and cannot
// have its own formatter). A removed device's removal reason is appended.
struct DX12HRWrap
{
  constexpr explicit DX12HRWrap(HRESULT hr) : m_hr(hr) {}
  const HRESULT m_hr;
};

}  // namespace DX12

template <>
struct fmt::formatter<DX12::DX12HRWrap>
{
  constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

  template <typename FormatContext>
  auto format(const DX12::DX12HRWrap& wrapped, FormatContext& ctx) const
  {
    ID3D12Device* const device =
        DX12::g_dx_context != nullptr ? DX12::g_dx_context->GetDevice() : nullptr;
    if (wrapped.m_hr != DXGI_ERROR_DEVICE_REMOVED || device == nullptr)
      return fmt::format_to(ctx.out(), "{}", Common::HRWrap(wrapped.m_hr));

    const HRESULT reason = device->GetDeviceRemovedReason();
    return fmt::format_to(ctx.out(), "{}\nDevice removal reason: {}", Common::HRWrap(wrapped.m_hr),
                          Common::HRWrap(reason));
  }
};
