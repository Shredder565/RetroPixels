// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3DCommon/D3DCommon.h"

#include <array>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "Common/Assert.h"
#include "Common/DynamicLibrary.h"
#include "Common/HRWrap.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoConfig.h"

namespace D3DCommon
{
pD3DCompile d3d_compile;

namespace
{
using FactoryProc = HRESULT (*)(REFIID riid, void** out_factory);
using Factory2Proc = HRESULT (*)(UINT flags, REFIID riid, void** out_factory);

// Everything that lives between LoadLibraries and UnloadLibraries.
struct RuntimeLibraries
{
  Common::DynamicLibrary dxgi;
  Common::DynamicLibrary compiler;
  FactoryProc make_factory = nullptr;
  Factory2Proc make_factory2 = nullptr;  // Windows 8.1 and later only
  bool ready = false;
};

RuntimeLibraries s_runtime;

// Per-format DXGI choices, indexed by AbstractTextureFormat. UNKNOWN in the
// view columns means the format cannot be used that way.
struct FormatRow
{
  DXGI_FORMAT storage;           // texture that is only sampled
  DXGI_FORMAT storage_typeless;  // texture that is also a render or depth target
  DXGI_FORMAT shader_view;
  DXGI_FORMAT target_view;
  DXGI_FORMAT target_view_uint;
  DXGI_FORMAT depth_view;
};

constexpr DXGI_FORMAT NONE = DXGI_FORMAT_UNKNOWN;

constexpr std::array<FormatRow, 14> FORMAT_ROWS = {{
    // RGBA8
    {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
     DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UINT, NONE},
    // BGRA8
    {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM,
     DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, NONE},
    // RGB10_A2
    {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_TYPELESS,
     DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
     NONE},
    // RGBA16F
    {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_TYPELESS,
     DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
     DXGI_FORMAT_R16G16B16A16_FLOAT, NONE},
    // DXT1, DXT3, DXT5, BPTC: block-compressed, never targets
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, NONE, NONE, NONE},
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, NONE, NONE, NONE},
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, NONE, NONE, NONE},
    {DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM, NONE, NONE, NONE},
    // R16
    {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM,
     DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UINT, NONE},
    // D16
    {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, NONE, NONE,
     DXGI_FORMAT_D16_UNORM},
    // D24_S8
    {DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
     NONE, NONE, DXGI_FORMAT_D24_UNORM_S8_UINT},
    // R32F
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT,
     DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, NONE},
    // D32F
    {DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, NONE, NONE,
     DXGI_FORMAT_D32_FLOAT},
    // D32F_S8
    {DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
     DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, NONE, NONE, DXGI_FORMAT_D32_FLOAT_S8X24_UINT},
}};

static_assert(static_cast<size_t>(AbstractTextureFormat::Undefined) == FORMAT_ROWS.size());

const FormatRow* FindRow(AbstractTextureFormat format)
{
  const auto index = static_cast<size_t>(format);
  return index < FORMAT_ROWS.size() ? &FORMAT_ROWS[index] : nullptr;
}

// DXGI formats that map back to a renderer format. Formats not listed here
// (including the depth views' colour aliases) are Undefined.
struct ReverseEntry
{
  DXGI_FORMAT dxgi;
  AbstractTextureFormat format;
};

constexpr std::array<ReverseEntry, 22> REVERSE_ROWS = {{
    {DXGI_FORMAT_R8G8B8A8_UINT, AbstractTextureFormat::RGBA8},
    {DXGI_FORMAT_R8G8B8A8_UNORM, AbstractTextureFormat::RGBA8},
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, AbstractTextureFormat::RGBA8},
    {DXGI_FORMAT_B8G8R8A8_UNORM, AbstractTextureFormat::BGRA8},
    {DXGI_FORMAT_B8G8R8A8_TYPELESS, AbstractTextureFormat::BGRA8},
    {DXGI_FORMAT_R10G10B10A2_UNORM, AbstractTextureFormat::RGB10_A2},
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, AbstractTextureFormat::RGB10_A2},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, AbstractTextureFormat::RGBA16F},
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, AbstractTextureFormat::RGBA16F},
    {DXGI_FORMAT_R16_UINT, AbstractTextureFormat::R16},
    {DXGI_FORMAT_R16_UNORM, AbstractTextureFormat::R16},
    {DXGI_FORMAT_R16_TYPELESS, AbstractTextureFormat::R16},
    {DXGI_FORMAT_R32_FLOAT, AbstractTextureFormat::R32F},
    {DXGI_FORMAT_R32_TYPELESS, AbstractTextureFormat::R32F},
    {DXGI_FORMAT_D16_UNORM, AbstractTextureFormat::D16},
    {DXGI_FORMAT_D24_UNORM_S8_UINT, AbstractTextureFormat::D24_S8},
    {DXGI_FORMAT_D32_FLOAT, AbstractTextureFormat::D32F},
    {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, AbstractTextureFormat::D32F_S8},
    {DXGI_FORMAT_BC1_UNORM, AbstractTextureFormat::DXT1},
    {DXGI_FORMAT_BC2_UNORM, AbstractTextureFormat::DXT3},
    {DXGI_FORMAT_BC3_UNORM, AbstractTextureFormat::DXT5},
    {DXGI_FORMAT_BC7_UNORM, AbstractTextureFormat::BPTC},
}};

// Gives an ID3D11 or ID3D12 child its debug-layer name, if it is that kind of object.
template <typename Child>
bool TryNameChild(IUnknown* object, std::string_view name)
{
  Microsoft::WRL::ComPtr<Child> child;
  if (FAILED(object->QueryInterface(IID_PPV_ARGS(&child))))
    return false;
  child->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data());
  return true;
}
}  // namespace

bool LoadLibraries()
{
  if (s_runtime.ready)
    return true;

  if (!s_runtime.dxgi.Open("dxgi.dll"))
  {
    PanicAlertFmtT("Failed to load dxgi.dll");
    return false;
  }

  if (!s_runtime.compiler.Open(D3DCOMPILER_DLL_A))
  {
    PanicAlertFmtT("Failed to load {0}. If you are using Windows 7, try installing the "
                   "KB4019990 update package.",
                   D3DCOMPILER_DLL_A);
    s_runtime.dxgi.Close();
    return false;
  }

  // D3DCompile and CreateDXGIFactory are mandatory; the second is not looked
  // up when the first is missing.
  const bool have_required = s_runtime.compiler.GetSymbol("D3DCompile", &d3d_compile) &&
                             s_runtime.dxgi.GetSymbol("CreateDXGIFactory", &s_runtime.make_factory);
  if (!have_required)
  {
    PanicAlertFmtT("Failed to find one or more D3D symbols");
    s_runtime.compiler.Close();
    s_runtime.dxgi.Close();
    return false;
  }

  // CreateDXGIFactory2 is only used for the debug flag; older systems lack it.
  s_runtime.dxgi.GetSymbol("CreateDXGIFactory2", &s_runtime.make_factory2);
  s_runtime.ready = true;
  return true;
}

void UnloadLibraries()
{
  s_runtime.make_factory = nullptr;
  s_runtime.make_factory2 = nullptr;
  d3d_compile = nullptr;
  s_runtime.compiler.Close();
  s_runtime.dxgi.Close();
  s_runtime.ready = false;
}

Microsoft::WRL::ComPtr<IDXGIFactory> CreateDXGIFactory(bool debug_device)
{
  Microsoft::WRL::ComPtr<IDXGIFactory> factory;

  if (s_runtime.make_factory2)
  {
    const UINT flags = debug_device ? DXGI_CREATE_FACTORY_DEBUG : 0;
    if (SUCCEEDED(s_runtime.make_factory2(flags, IID_PPV_ARGS(&factory))))
      return factory;
  }

  // The original entry point cannot make a debug factory.
  const HRESULT result = s_runtime.make_factory(IID_PPV_ARGS(&factory));
  if (FAILED(result))
  {
    PanicAlertFmt("CreateDXGIFactory() failed: {}", Common::HRWrap(result));
    return nullptr;
  }
  return factory;
}

std::vector<std::string> GetAdapterNames()
{
  Microsoft::WRL::ComPtr<IDXGIFactory> factory;
  if (FAILED(s_runtime.make_factory(IID_PPV_ARGS(&factory))))
    return {};

  std::vector<std::string> names;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  for (;;)
  {
    const auto next = static_cast<UINT>(names.size());
    if (factory->EnumAdapters(next, &adapter) == DXGI_ERROR_NOT_FOUND)
      break;

    // An adapter whose description cannot be read still takes a slot, so the
    // list index stays the DXGI adapter index.
    DXGI_ADAPTER_DESC info;
    names.emplace_back(SUCCEEDED(adapter->GetDesc(&info)) ? WStringToUTF8(info.Description) :
                                                            std::string());
  }
  return names;
}

DXGI_FORMAT GetDXGIFormatForAbstractFormat(AbstractTextureFormat format, bool typeless)
{
  if (const FormatRow* row = FindRow(format))
    return typeless ? row->storage_typeless : row->storage;

  PanicAlertFmt("Unhandled texture format.");
  return DXGI_FORMAT_R8G8B8A8_UNORM;
}

DXGI_FORMAT GetSRVFormatForAbstractFormat(AbstractTextureFormat format)
{
  if (const FormatRow* row = FindRow(format))
    return row->shader_view;

  PanicAlertFmt("Unhandled SRV format");
  return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT GetRTVFormatForAbstractFormat(AbstractTextureFormat format, bool integer)
{
  const FormatRow* row = FindRow(format);
  if (row && row->target_view != NONE)
    return integer ? row->target_view_uint : row->target_view;

  PanicAlertFmt("Unhandled RTV format");
  return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT GetDSVFormatForAbstractFormat(AbstractTextureFormat format)
{
  const FormatRow* row = FindRow(format);
  if (row && row->depth_view != NONE)
    return row->depth_view;

  PanicAlertFmt("Unhandled DSV format");
  return DXGI_FORMAT_UNKNOWN;
}

AbstractTextureFormat GetAbstractFormatForDXGIFormat(DXGI_FORMAT format)
{
  for (const ReverseEntry& entry : REVERSE_ROWS)
  {
    if (entry.dxgi == format)
      return entry.format;
  }
  return AbstractTextureFormat::Undefined;
}

void SetDebugObjectName(IUnknown* resource, std::string_view name)
{
  if (!g_ActiveConfig.bEnableValidationLayer)
    return;

  if (!TryNameChild<ID3D11DeviceChild>(resource, name))
    TryNameChild<ID3D12DeviceChild>(resource, name);
}
}  // namespace D3DCommon
