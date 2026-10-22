// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D/D3DBase.h"

#include <algorithm>
#include <array>

#include "Common/CommonTypes.h"
#include "Common/DynamicLibrary.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VideoConfig.h"

namespace DX11
{
namespace
{
Common::DynamicLibrary s_d3d11_dll;

// Accepted feature levels, best first. 10_0 is the minimum the backend runs on.
constexpr std::array<D3D_FEATURE_LEVEL, 3> FEATURE_LEVELS = {
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
};

HRESULT MakeDevice(PFN_D3D11_CREATE_DEVICE create_device, IDXGIAdapter* adapter, UINT flags,
                   ID3D11Device** out_device, D3D_FEATURE_LEVEL* out_level,
                   ID3D11DeviceContext** out_context)
{
  return create_device(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, FEATURE_LEVELS.data(),
                       static_cast<UINT>(FEATURE_LEVELS.size()), D3D11_SDK_VERSION, out_device,
                       out_level, out_context);
}

bool LookUpCreateDevice(Common::DynamicLibrary& library, PFN_D3D11_CREATE_DEVICE* out)
{
  return library.Open("d3d11.dll") && library.GetSymbol("D3D11CreateDevice", out);
}

// A short-lived device for capability queries made before the backend starts.
// The library is declared first so it outlives the device.
struct ProbeDevice
{
  Common::DynamicLibrary library;
  ComPtr<ID3D11Device> device;

  // Pass a null level pointer when the level is not wanted. With as_11_1 the
  // 11.1 interface is queried while the factory and adapter are still held.
  bool Open(u32 adapter_index, D3D_FEATURE_LEVEL* level, ComPtr<ID3D11Device1>* as_11_1 = nullptr)
  {
    ComPtr<IDXGIFactory> factory = D3DCommon::CreateDXGIFactory(false);
    if (!factory)
      return false;

    // Without the requested adapter the default one is used.
    ComPtr<IDXGIAdapter> adapter;
    factory->EnumAdapters(adapter_index, &adapter);

    PFN_D3D11_CREATE_DEVICE create_device;
    if (!LookUpCreateDevice(library, &create_device))
      return false;

    if (FAILED(MakeDevice(create_device, adapter.Get(), 0, device.ReleaseAndGetAddressOf(), level,
                          nullptr)))
    {
      return false;
    }
    return !as_11_1 || SUCCEEDED(device.As(as_11_1));
  }
};
}  // namespace

namespace D3D
{
ComPtr<IDXGIFactory> dxgi_factory;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11Device1> device1;
ComPtr<ID3D11DeviceContext> context;
D3D_FEATURE_LEVEL feature_level;

namespace
{
ComPtr<ID3D11Debug> s_debug_interface;

void ConfigureInfoQueue()
{
  ComPtr<ID3D11InfoQueue> queue;
  if (FAILED(s_debug_interface.As(&queue)))
    return;

  // Stop in the debugger on errors.
  queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, true);
  queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, true);

  // Renaming an object (SetDebugObjectName on a reused object) is expected.
  D3D11_MESSAGE_ID muted[] = {D3D11_MESSAGE_ID_SETPRIVATEDATA_CHANGINGPARAMS};
  D3D11_INFO_QUEUE_FILTER filter = {};
  filter.DenyList.NumIDs = static_cast<UINT>(std::size(muted));
  filter.DenyList.pIDList = muted;
  queue->AddStorageFilterEntries(&filter);
}

// The debug runtime may be missing (it ships with the SDK/graphics tools);
// a failure here just means a normal device is made instead.
HRESULT TryCreateDebugDevice(PFN_D3D11_CREATE_DEVICE create_device, IDXGIAdapter* adapter)
{
  HRESULT result =
      MakeDevice(create_device, adapter, D3D11_CREATE_DEVICE_DEBUG, device.ReleaseAndGetAddressOf(),
                 &feature_level, context.ReleaseAndGetAddressOf());
  if (SUCCEEDED(result))
    result = device.As(&s_debug_interface);

  if (SUCCEEDED(result))
    ConfigureInfoQueue();
  else
    WARN_LOG_FMT(VIDEO, "Debug layer requested but not available: {}", DX11HRWrap(result));
  return result;
}

void ReleaseRuntime()
{
  D3DCommon::UnloadLibraries();
  s_d3d11_dll.Close();
}
}  // namespace

bool Create(u32 adapter_index, bool enable_debug_layer)
{
  PFN_D3D11_CREATE_DEVICE create_device;
  if (!LookUpCreateDevice(s_d3d11_dll, &create_device))
  {
    PanicAlertFmtT("Failed to load d3d11.dll");
    s_d3d11_dll.Close();
    return false;
  }

  if (!D3DCommon::LoadLibraries())
  {
    s_d3d11_dll.Close();
    return false;
  }

  dxgi_factory = D3DCommon::CreateDXGIFactory(enable_debug_layer);
  if (!dxgi_factory)
  {
    PanicAlertFmtT("Failed to create DXGI factory");
    ReleaseRuntime();
    return false;
  }

  ComPtr<IDXGIAdapter> adapter;
  HRESULT result = dxgi_factory->EnumAdapters(adapter_index, &adapter);
  if (FAILED(result))
  {
    WARN_LOG_FMT(VIDEO, "Adapter {} not found, using default: {}", adapter_index,
                 DX11HRWrap(result));
    adapter = nullptr;
  }

  bool have_device = false;
  if (enable_debug_layer)
    have_device = SUCCEEDED(TryCreateDebugDevice(create_device, adapter.Get()));
  if (!have_device)
  {
    result = MakeDevice(create_device, adapter.Get(), 0, device.ReleaseAndGetAddressOf(),
                        &feature_level, context.ReleaseAndGetAddressOf());
    if (FAILED(result))
    {
      PanicAlertFmtT("Failed to initialize Direct3D.\nMake sure your video card supports at "
                     "least D3D 10.0\n{0}",
                     DX11HRWrap(result));
      dxgi_factory.Reset();
      ReleaseRuntime();
      return false;
    }
  }

  result = device.As(&device1);
  if (FAILED(result))
  {
    WARN_LOG_FMT(VIDEO,
                 "Missing Direct3D 11.1 support. Logical operations will not be supported.\n{}",
                 DX11HRWrap(result));
  }

  stateman = std::make_unique<StateManager>();
  return true;
}

void Destroy()
{
  stateman.reset();

  context->ClearState();
  context->Flush();
  context.Reset();
  device1.Reset();

  ULONG leaked = device.Reset();
  if (s_debug_interface)
  {
    // The debug interface itself holds one device reference.
    --leaked;
    if (leaked != 0)
    {
      // Lists the live objects (including runtime-internal ones) in the debug output.
      constexpr auto report_flags =
          static_cast<D3D11_RLDO_FLAGS>(D3D11_RLDO_SUMMARY | D3D11_RLDO_DETAIL);
      s_debug_interface->ReportLiveDeviceObjects(report_flags);
    }
    s_debug_interface.Reset();
  }

  if (leaked != 0)
    ERROR_LOG_FMT(VIDEO, "Unreleased references: {}.", leaked);
  else
    NOTICE_LOG_FMT(VIDEO, "Successfully released all device references!");

  dxgi_factory.Reset();
  ReleaseRuntime();
}

std::vector<u32> GetAAModes(u32 adapter_index)
{
  ProbeDevice probe;
  probe.device = device;
  D3D_FEATURE_LEVEL level = feature_level;
  if (!probe.device && !probe.Open(adapter_index, &level))
    return {};

  // Feature level 10_0 cannot have a multisampled depth buffer that is also
  // sampled, so no MSAA there.
  if (level == D3D_FEATURE_LEVEL_10_0)
    return {};

  const DXGI_FORMAT efb_format =
      D3DCommon::GetDXGIFormatForAbstractFormat(FramebufferManager::GetEFBColorFormat(), false);
  std::vector<u32> modes;
  for (u32 count = 1; count <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; ++count)
  {
    UINT levels = 0;
    const HRESULT result = probe.device->CheckMultisampleQualityLevels(efb_format, count, &levels);
    if (SUCCEEDED(result) && levels > 0)
      modes.push_back(count);
  }
  return modes;
}

bool SupportsTextureFormat(DXGI_FORMAT format)
{
  UINT support;
  if (FAILED(device->CheckFormatSupport(format, &support)))
    return false;
  return (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0;
}

bool SupportsLogicOp(u32 adapter_index)
{
  ProbeDevice probe;
  ComPtr<ID3D11Device1> device_11_1 = device1;
  if (!device)
  {
    if (!probe.Open(adapter_index, nullptr, &device_11_1))
      return false;
  }

  if (!device_11_1)
    return false;

  D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
  if (FAILED(
          device_11_1->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))))
  {
    return false;
  }
  return options.OutputMergerLogicOp != FALSE;
}

}  // namespace D3D

}  // namespace DX11
