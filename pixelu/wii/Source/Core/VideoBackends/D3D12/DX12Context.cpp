// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D12/DX12Context.h"

#include <algorithm>
#include <array>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <queue>
#include <vector>

#include "Common/Assert.h"
#include "Common/DynamicLibrary.h"
#include "Common/StringUtil.h"

#include "VideoBackends/D3D12/Common.h"
#include "VideoBackends/D3D12/D3D12StreamBuffer.h"
#include "VideoBackends/D3D12/DescriptorHeapManager.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VideoConfig.h"

namespace DX12
{
std::unique_ptr<DXContext> g_dx_context;

namespace
{
// d3d12.dll and the entry points the backend uses from it.
struct D3D12Runtime
{
  Common::DynamicLibrary library;
  PFN_D3D12_CREATE_DEVICE create_device = nullptr;
  PFN_D3D12_GET_DEBUG_INTERFACE get_debug_interface = nullptr;
  PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize_root_signature = nullptr;

  bool Load()
  {
    return library.Open("d3d12.dll") && library.GetSymbol("D3D12CreateDevice", &create_device) &&
           library.GetSymbol("D3D12GetDebugInterface", &get_debug_interface) &&
           library.GetSymbol("D3D12SerializeRootSignature", &serialize_root_signature);
  }

  void Unload()
  {
    serialize_root_signature = nullptr;
    get_debug_interface = nullptr;
    create_device = nullptr;
    library.Close();
  }
};

D3D12Runtime s_runtime;

// Collects root parameters; each descriptor table owns one range.
class RootSignatureBuilder
{
public:
  void AddCBV(u32 reg, D3D12_SHADER_VISIBILITY visibility)
  {
    D3D12_ROOT_PARAMETER& param = Next(D3D12_ROOT_PARAMETER_TYPE_CBV, visibility);
    param.Descriptor.ShaderRegister = reg;
    param.Descriptor.RegisterSpace = 0;
  }

  void AddConstants(u32 reg, u32 count, D3D12_SHADER_VISIBILITY visibility)
  {
    D3D12_ROOT_PARAMETER& param = Next(D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, visibility);
    param.Constants.ShaderRegister = reg;
    param.Constants.RegisterSpace = 0;
    param.Constants.Num32BitValues = count;
  }

  void AddTable(D3D12_DESCRIPTOR_RANGE_TYPE type, u32 first_reg, u32 count,
                D3D12_SHADER_VISIBILITY visibility)
  {
    D3D12_DESCRIPTOR_RANGE& range = m_ranges[m_count];
    range.RangeType = type;
    range.NumDescriptors = count;
    range.BaseShaderRegister = first_reg;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER& param = Next(D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, visibility);
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
  }

  // Serializes and creates the signature. A creation failure is only
  // asserted; the result then is true with a null signature.
  bool Build(ID3D12Device* device, ID3D12RootSignature** out) const
  {
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = m_count;
    desc.pParameters = m_params.data();
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    HRESULT result =
        s_runtime.serialize_root_signature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (FAILED(result))
    {
      PanicAlertFmt("Failed to serialize root signature: {}\n{}",
                    static_cast<const char*>(errors->GetBufferPointer()), DX12HRWrap(result));
      return false;
    }

    result = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(out));
    ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create root signature: {}", DX12HRWrap(result));
    return true;
  }

private:
  D3D12_ROOT_PARAMETER& Next(D3D12_ROOT_PARAMETER_TYPE type, D3D12_SHADER_VISIBILITY visibility)
  {
    D3D12_ROOT_PARAMETER& param = m_params[m_count++];
    param.ParameterType = type;
    param.ShaderVisibility = visibility;
    return param;
  }

  std::array<D3D12_ROOT_PARAMETER, NUM_ROOT_PARAMETERS> m_params{};
  std::array<D3D12_DESCRIPTOR_RANGE, NUM_ROOT_PARAMETERS> m_ranges{};
  u32 m_count = 0;
};
}  // namespace

DXContext::DXContext() = default;

DXContext::~DXContext()
{
  if (m_fence_event)
    CloseHandle(m_fence_event);
}

std::vector<u32> DXContext::GetAAModes(u32 adapter_index)
{
  // The library is declared first so it is closed after the device is gone.
  Common::DynamicLibrary probe_library;
  ComPtr<ID3D12Device> device = g_dx_context ? g_dx_context->m_device : nullptr;
  if (!device)
  {
    ComPtr<IDXGIFactory> factory = D3DCommon::CreateDXGIFactory(false);
    if (!factory)
      return {};

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(factory->EnumAdapters(adapter_index, &adapter)))
      return {};

    PFN_D3D12_CREATE_DEVICE create_device;
    if (!probe_library.Open("d3d12.dll") ||
        !probe_library.GetSymbol("D3D12CreateDevice", &create_device))
    {
      return {};
    }

    if (FAILED(create_device(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
      return {};
  }

  // Counts 1 to 31; the query result is not checked, only the level count.
  const DXGI_FORMAT efb_format =
      D3DCommon::GetDXGIFormatForAbstractFormat(FramebufferManager::GetEFBColorFormat(), false);
  std::vector<u32> modes;
  for (u32 count = 1; count < D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT; ++count)
  {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS query = {};
    query.Format = efb_format;
    query.SampleCount = count;
    device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &query, sizeof(query));
    if (query.NumQualityLevels > 0)
      modes.push_back(count);
  }
  return modes;
}

bool DXContext::SupportsTextureFormat(DXGI_FORMAT format)
{
  const u32 needed = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TEXTURECUBE |
                     D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;

  D3D12_FEATURE_DATA_FORMAT_SUPPORT query = {};
  query.Format = format;
  if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &query, sizeof(query))))
    return false;
  return (query.Support1 & needed) == needed;
}

bool DXContext::Create(u32 adapter_index, bool enable_debug_layer)
{
  ASSERT(!g_dx_context);
  if (!s_runtime.Load())
  {
    PanicAlertFmtT("d3d12.dll could not be loaded.");
    s_runtime.library.Close();
    return false;
  }

  if (!D3DCommon::LoadLibraries())
  {
    s_runtime.library.Close();
    return false;
  }

  g_dx_context.reset(new DXContext());
  DXContext& context = *g_dx_context;
  const bool created = context.CreateDXGIFactory(enable_debug_layer) &&
                       context.CreateDevice(adapter_index, enable_debug_layer) &&
                       context.CreateCommandQueue() && context.CreateFence();
  if (!created)
  {
    Destroy();
    return false;
  }
  return true;
}

bool DXContext::CreateGlobalResources()
{
  return CreateDescriptorHeaps() && CreateRootSignatures() && CreateTextureUploadBuffer() &&
         CreateCommandLists();
}

void DXContext::Destroy()
{
  g_dx_context.reset();
  s_runtime.Unload();
  D3DCommon::UnloadLibraries();
}

bool DXContext::CreateDXGIFactory(bool enable_debug_layer)
{
  m_dxgi_factory = D3DCommon::CreateDXGIFactory(enable_debug_layer);
  return m_dxgi_factory != nullptr;
}

bool DXContext::CreateDevice(u32 adapter_index, bool enable_debug_layer)
{
  ComPtr<IDXGIAdapter> adapter;
  HRESULT result = m_dxgi_factory->EnumAdapters(adapter_index, &adapter);
  if (FAILED(result))
  {
    ERROR_LOG_FMT(VIDEO, "Adapter {} not found, using default: {}", adapter_index,
                  DX12HRWrap(result));
    adapter = nullptr;
  }

  // The debug layer is only there when the Graphics Tools feature is installed.
  bool debugging = enable_debug_layer;
  if (debugging)
  {
    result = s_runtime.get_debug_interface(IID_PPV_ARGS(&m_debug_interface));
    if (SUCCEEDED(result))
    {
      m_debug_interface->EnableDebugLayer();
    }
    else
    {
      ERROR_LOG_FMT(VIDEO, "Debug layer requested but not available: {}", DX12HRWrap(result));
      debugging = false;
    }
  }

  result = s_runtime.create_device(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create D3D12 device: {}", DX12HRWrap(result));
  if (FAILED(result))
    return false;

  if (!debugging)
    return true;

  ComPtr<ID3D12InfoQueue> queue;
  if (FAILED(m_device.As(&queue)))
    return true;

  queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
  queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

  // Messages the backend triggers on purpose.
  std::array<D3D12_MESSAGE_ID, 5> muted = {
      D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
      D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
      D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET,
      D3D12_MESSAGE_ID_CREATEINPUTLAYOUT_TYPE_MISMATCH,
      D3D12_MESSAGE_ID_DRAW_EMPTY_SCISSOR_RECTANGLE,
  };
  D3D12_INFO_QUEUE_FILTER filter = {};
  filter.DenyList.NumIDs = static_cast<UINT>(muted.size());
  filter.DenyList.pIDList = muted.data();
  queue->PushStorageFilter(&filter);
  return true;
}

bool DXContext::CreateCommandQueue()
{
  D3D12_COMMAND_QUEUE_DESC desc = {};
  desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

  const HRESULT result = m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_command_queue));
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create command queue: {}", DX12HRWrap(result));
  return SUCCEEDED(result);
}

bool DXContext::CreateFence()
{
  const HRESULT result =
      m_device->CreateFence(m_completed_fence_value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create fence: {}", DX12HRWrap(result));
  if (FAILED(result))
    return false;

  // Auto-reset event for blocking fence waits.
  m_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  ASSERT_MSG(VIDEO, m_fence_event != nullptr, "Failed to create fence event");
  return m_fence_event != nullptr;
}

bool DXContext::CreateDescriptorHeaps()
{
  constexpr u32 SRV_SLOTS = 16384;
  constexpr u32 RTV_SLOTS = 8192;
  constexpr u32 DSV_SLOTS = 128;
  constexpr u32 SAMPLER_SLOTS = 16384;

  ID3D12Device* const device = m_device.Get();
  const bool created =
      m_descriptor_heap_manager.Create(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRV_SLOTS) &&
      m_rtv_heap_manager.Create(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RTV_SLOTS) &&
      m_dsv_heap_manager.Create(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DSV_SLOTS) &&
      m_sampler_heap_manager.Create(device, SAMPLER_SLOTS);
  if (!created)
    return false;

  m_gpu_descriptor_heaps[1] = m_sampler_heap_manager.GetDescriptorHeap();

  // A view of no resource, bound in place of missing textures.
  if (!m_descriptor_heap_manager.Allocate(&m_null_srv_descriptor))
  {
    PanicAlertFmt("Failed to allocate null descriptor");
    return false;
  }

  D3D12_SHADER_RESOURCE_VIEW_DESC null_view = {};
  null_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  null_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  device->CreateShaderResourceView(nullptr, &null_view, m_null_srv_descriptor.cpu_handle);
  return true;
}

bool DXContext::CreateRootSignatures()
{
  return CreateGXRootSignature() && CreateUtilityRootSignature() && CreateComputeRootSignature();
}

bool DXContext::CreateGXRootSignature()
{
  constexpr auto PS = D3D12_SHADER_VISIBILITY_PIXEL;
  constexpr auto VS = D3D12_SHADER_VISIBILITY_VERTEX;
  constexpr u32 SLOTS = VideoCommon::MAX_PIXEL_SHADER_SAMPLERS;

  // In ROOT_PARAMETER order.
  RootSignatureBuilder sig;
  sig.AddCBV(0, PS);                                                // pixel constants
  sig.AddTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, SLOTS, PS);      // textures
  sig.AddTable(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, SLOTS, PS);  // samplers
  sig.AddCBV(0, VS);                                                // vertex constants
  sig.AddCBV(1, VS);                                                // vertex constants, second view
  sig.AddCBV(2, VS);                                                // custom constants
  // Geometry constants, or the expansion constants when the vertex shader
  // expands lines and points.
  if (g_ActiveConfig.UseVSForLinePointExpand())
    sig.AddCBV(3, VS);
  else
    sig.AddCBV(0, D3D12_SHADER_VISIBILITY_GEOMETRY);
  sig.AddTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 1, VS);  // vertex texel buffer
  sig.AddConstants(4, 1, VS);                               // base vertex
  sig.AddCBV(2, PS);                                        // custom pixel constants

  // The optional parameters are packed: per-pixel lighting takes the
  // bounding-box slot when the bounding box is off.
  if (g_ActiveConfig.bBBoxEnable)
    sig.AddTable(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 1, PS);
  if (g_ActiveConfig.bEnablePixelLighting)
    sig.AddCBV(1, PS);

  return sig.Build(m_device.Get(), &m_gx_root_signature);
}

bool DXContext::CreateUtilityRootSignature()
{
  RootSignatureBuilder sig;
  sig.AddCBV(0, D3D12_SHADER_VISIBILITY_ALL);
  sig.AddTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 8, D3D12_SHADER_VISIBILITY_PIXEL);
  sig.AddTable(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 8, D3D12_SHADER_VISIBILITY_PIXEL);
  return sig.Build(m_device.Get(), &m_utility_root_signature);
}

bool DXContext::CreateComputeRootSignature()
{
  // In CS_ROOT_PARAMETERS order.
  constexpr auto ALL = D3D12_SHADER_VISIBILITY_ALL;
  RootSignatureBuilder sig;
  sig.AddCBV(0, ALL);
  sig.AddTable(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 8, ALL);
  sig.AddTable(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 0, 8, ALL);
  sig.AddTable(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1, ALL);
  return sig.Build(m_device.Get(), &m_compute_root_signature);
}

bool DXContext::CreateTextureUploadBuffer()
{
  if (m_texture_upload_buffer.AllocateBuffer(TEXTURE_UPLOAD_BUFFER_SIZE))
    return true;

  PanicAlertFmt("Failed to create texture upload buffer");
  return false;
}

bool DXContext::CreateCommandLists()
{
  // Up to 8192 draws a frame, eight descriptors each.
  constexpr u32 PER_LIST_DESCRIPTORS = 8192 * 8;

  for (CommandListResources& list : m_command_lists)
  {
    HRESULT result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&list.command_allocator));
    ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create command allocator: {}",
               DX12HRWrap(result));
    if (FAILED(result))
      return false;

    result =
        m_device->CreateCommandList(1, D3D12_COMMAND_LIST_TYPE_DIRECT, list.command_allocator.Get(),
                                    nullptr, IID_PPV_ARGS(&list.command_list));
    ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create command list: {}", DX12HRWrap(result));
    if (FAILED(result))
      return false;

    // Lists are created open; closed here because each is reset before use.
    result = list.command_list->Close();
    ASSERT_MSG(VIDEO, SUCCEEDED(result), "Closing new command list failed: {}", DX12HRWrap(result));
    if (FAILED(result))
      return false;

    if (!list.descriptor_allocator.Create(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                          PER_LIST_DESCRIPTORS))
    {
      return false;
    }
    if (!list.sampler_allocator.Create(m_device.Get()))
      return false;
  }

  MoveToNextCommandList();
  return true;
}

void DXContext::MoveToNextCommandList()
{
  m_current_command_list = (m_current_command_list + 1) % NUM_COMMAND_LISTS;
  ++m_current_fence_value;

  // The list's previous submission has to be finished before it is reused.
  CommandListResources& list = m_command_lists[m_current_command_list];
  WaitForFence(list.ready_fence_value);

  list.command_allocator->Reset();
  list.command_list->Reset(list.command_allocator.Get(), nullptr);
  list.descriptor_allocator.Reset();
  if (list.sampler_allocator.ShouldReset())
    list.sampler_allocator.Reset();

  m_gpu_descriptor_heaps[0] = list.descriptor_allocator.GetDescriptorHeap();
  m_gpu_descriptor_heaps[1] = list.sampler_allocator.GetDescriptorHeap();
  list.ready_fence_value = m_current_fence_value;
}

void DXContext::ExecuteCommandList(bool wait_for_completion)
{
  CommandListResources& list = m_command_lists[m_current_command_list];

  HRESULT result = list.command_list->Close();
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to close command list: {}", DX12HRWrap(result));

  ID3D12CommandList* const submit[] = {list.command_list.Get()};
  m_command_queue->ExecuteCommandLists(1, submit);

  // The fence reaches this list's value when the GPU is done with it.
  result = m_command_queue->Signal(m_fence.Get(), m_current_fence_value);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to signal fence: {}", DX12HRWrap(result));

  MoveToNextCommandList();
  if (wait_for_completion)
    WaitForFence(list.ready_fence_value);
}

void DXContext::DeferResourceDestruction(ID3D12Resource* resource)
{
  resource->AddRef();
  m_command_lists[m_current_command_list].pending_resources.push_back(resource);
}

void DXContext::DeferDescriptorDestruction(DescriptorHeapManager& manager, u32 index)
{
  m_command_lists[m_current_command_list].pending_descriptors.emplace_back(manager, index);
}

void DXContext::ResetSamplerAllocators()
{
  for (CommandListResources& list : m_command_lists)
    list.sampler_allocator.Reset();
}

void DXContext::RecreateGXRootSignature()
{
  m_gx_root_signature.Reset();
  if (!CreateGXRootSignature())
    PanicAlertFmt("Failed to re-create GX root signature.");
}

void DXContext::DestroyPendingResources(CommandListResources& cmdlist)
{
  // Descriptors first, then the resources they described.
  for (auto& [manager, index] : cmdlist.pending_descriptors)
    manager.Free(index);
  cmdlist.pending_descriptors.clear();

  for (ID3D12Resource* resource : cmdlist.pending_resources)
    resource->Release();
  cmdlist.pending_resources.clear();
}

void DXContext::WaitForFence(u64 fence)
{
  if (fence <= m_completed_fence_value)
    return;

  // Poll first; block on the event only when the GPU is not there yet.
  m_completed_fence_value = m_fence->GetCompletedValue();
  if (m_completed_fence_value < fence)
  {
    const HRESULT result = m_fence->SetEventOnCompletion(fence, m_fence_event);
    ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to set fence event on completion: {}",
               DX12HRWrap(result));
    WaitForSingleObject(m_fence_event, INFINITE);
    m_completed_fence_value = m_fence->GetCompletedValue();
  }

  // Release what every finished list held, oldest list first, stopping at
  // the first one still running.
  for (u32 step = 1; step <= NUM_COMMAND_LISTS; ++step)
  {
    CommandListResources& list =
        m_command_lists[(m_current_command_list + step) % NUM_COMMAND_LISTS];
    if (m_completed_fence_value < list.ready_fence_value)
      break;
    DestroyPendingResources(list);
  }
}
}  // namespace DX12
