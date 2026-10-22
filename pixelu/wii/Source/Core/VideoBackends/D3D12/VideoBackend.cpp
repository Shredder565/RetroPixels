// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D12/VideoBackend.h"

#include <memory>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "Core/ConfigManager.h"

#include "VideoBackends/D3D12/Common.h"
#include "VideoBackends/D3D12/D3D12BoundingBox.h"
#include "VideoBackends/D3D12/D3D12Gfx.h"
#include "VideoBackends/D3D12/D3D12PerfQuery.h"
#include "VideoBackends/D3D12/D3D12SwapChain.h"
#include "VideoBackends/D3D12/D3D12VertexManager.h"
#include "VideoBackends/D3D12/DX12Context.h"

#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/ShaderCache.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace DX12
{
std::string VideoBackend::GetConfigName() const
{
  return CONFIG_NAME;
}

std::string VideoBackend::GetDisplayName() const
{
  return "Direct3D 12";
}

void VideoBackend::InitBackendInfo(const WindowSystemInfo& /*wsi*/)
{
  if (!D3DCommon::LoadLibraries())
    return;

  FillBackendInfo();
  D3DCommon::UnloadLibraries();
}

void VideoBackend::FillBackendInfo()
{
  auto& info = g_backend_info;
  info.api_type = APIType::D3D;

  // Supported on every D3D12 device.
  info.bSupportsExclusiveFullscreen = true;
  info.bSupportsDualSourceBlend = true;
  info.bSupportsPrimitiveRestart = true;
  info.bSupportsGeometryShaders = true;
  info.bSupportsGSInstancing = true;
  info.bSupportsComputeShaders = true;
  info.bSupportsEarlyZ = true;
  info.bSupportsBBox = true;
  info.bSupportsFragmentStoresAndAtomics = true;
  info.bSupportsLogicOp = true;
  info.bSupportsSSAA = true;
  info.bSupportsPaletteConversion = true;
  info.bSupportsPostProcessing = true;
  info.bSupportsClipControl = true;
  info.bSupportsDepthClamp = true;
  info.bSupportsGPUTextureDecoding = true;
  info.bSupportsCopyToVram = true;
  info.bSupportsBackgroundCompiling = true;
  info.bSupportsDepthReadback = true;
  info.bSupportsShaderBinaries = true;
  info.bSupportsPipelineCacheData = true;
  info.bSupportsCoarseDerivatives = true;
  info.bSupportsTextureQueryLevels = true;
  info.bSupportsLodBiasInSampler = true;
  info.bSupportsSettingObjectNames = true;
  info.bSupportsPartialMultisampleResolve = true;
  info.bSupportsDynamicVertexLoader = true;
  info.bSupportsVSLinePointExpand = true;
  info.bSupportsHDROutput = true;

  // Not offered by this backend.
  info.bUsesLowerLeftOrigin = false;
  info.bSupports3DVision = false;
  info.bSupportsBindingLayout = false;
  info.bSupportsReversedDepthRange = false;
  info.bSupportsMultithreading = false;
  info.bSupportsBitfield = false;
  info.bSupportsDynamicSamplerIndexing = false;
  info.bSupportsFramebufferFetch = false;
  info.bSupportsLargePoints = false;
  info.bSupportsPartialDepthCopies = false;

  // Compressed formats are off until a device can be asked (below).
  info.bSupportsST3CTextures = false;
  info.bSupportsBPTCTextures = false;

  // The sample counts may need a temporary device.
  info.Adapters = D3DCommon::GetAdapterNames();
  info.AAModes = DXContext::GetAAModes(static_cast<u32>(g_Config.iAdapter));

  if (!g_dx_context)
    return;

  info.bSupportsST3CTextures = g_dx_context->SupportsTextureFormat(DXGI_FORMAT_BC1_UNORM) &&
                               g_dx_context->SupportsTextureFormat(DXGI_FORMAT_BC2_UNORM) &&
                               g_dx_context->SupportsTextureFormat(DXGI_FORMAT_BC3_UNORM);
  info.bSupportsBPTCTextures = g_dx_context->SupportsTextureFormat(DXGI_FORMAT_BC7_UNORM);
}

bool VideoBackend::Initialize(const WindowSystemInfo& wsi)
{
  if (!DXContext::Create(static_cast<u32>(g_Config.iAdapter), g_Config.bEnableValidationLayer))
  {
    PanicAlertFmtT("Failed to create D3D12 context");
    return false;
  }

  FillBackendInfo();
  UpdateActiveConfig();

  // Undoes the steps so far when a later one fails.
  const auto give_up = [this] {
    DXContext::Destroy();
    ShutdownShared();
    return false;
  };

  if (!g_dx_context->CreateGlobalResources())
  {
    PanicAlertFmtT("Failed to create D3D12 global resources");
    return give_up();
  }

  // Headless runs have no window and so no swap chain.
  std::unique_ptr<SwapChain> swap_chain;
  if (wsi.render_surface)
  {
    swap_chain = SwapChain::Create(wsi);
    if (!swap_chain)
    {
      PanicAlertFmtT("Failed to create D3D swap chain");
      return give_up();
    }
  }

  auto gfx = std::make_unique<DX12::Gfx>(std::move(swap_chain), wsi.render_surface_scale);
  auto vertex_manager = std::make_unique<DX12::VertexManager>();
  auto perf_query = std::make_unique<DX12::PerfQuery>();
  auto bounding_box = std::make_unique<DX12::D3D12BoundingBox>();
  return InitializeShared(std::move(gfx), std::move(vertex_manager), std::move(perf_query),
                          std::move(bounding_box));
}

void VideoBackend::Shutdown()
{
  // Let the GPU finish before anything it uses is released, so the debug
  // layer has nothing to report.
  if (g_gfx)
    Gfx::GetInstance()->ExecuteCommandList(true);

  ShutdownShared();
  DXContext::Destroy();
}
}  // namespace DX12
