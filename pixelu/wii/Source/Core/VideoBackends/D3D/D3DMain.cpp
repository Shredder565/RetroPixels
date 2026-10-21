// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D/VideoBackend.h"

#include <memory>
#include <string>

#include "Common/Common.h"
#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DBoundingBox.h"
#include "VideoBackends/D3D/D3DGfx.h"
#include "VideoBackends/D3D/D3DPerfQuery.h"
#include "VideoBackends/D3D/D3DSwapChain.h"
#include "VideoBackends/D3D/D3DVertexManager.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/ShaderCache.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace DX11
{
std::string VideoBackend::GetConfigName() const
{
  return CONFIG_NAME;
}

std::string VideoBackend::GetDisplayName() const
{
  return _trans("Direct3D 11");
}

std::optional<std::string> VideoBackend::GetWarningMessage() const
{
  // Asked before the backend is chosen, so logic-op support is probed here
  // with a temporary device rather than read from the backend info.
  bool has_logic_ops = false;
  if (D3DCommon::LoadLibraries())
  {
    has_logic_ops = D3D::SupportsLogicOp(static_cast<u32>(g_Config.iAdapter));
    D3DCommon::UnloadLibraries();
  }

  if (has_logic_ops)
    return std::nullopt;

  return _trans("The Direct3D 11 renderer requires support for features not supported by your "
                "system configuration. You may still use this backend, but you will encounter "
                "graphical artifacts in certain games.\n"
                "\n"
                "Do you really want to switch to Direct3D 11? If unsure, select 'No'.");
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
  info.MaxTextureSize = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;

  // Fixed capabilities of the D3D11 path.
  info.bUsesLowerLeftOrigin = false;
  info.bSupportsExclusiveFullscreen = true;
  info.bSupportsDualSourceBlend = true;
  info.bSupportsPrimitiveRestart = true;
  info.bSupportsGeometryShaders = true;
  info.bSupportsComputeShaders = false;
  info.bSupports3DVision = true;
  info.bSupportsPostProcessing = true;
  info.bSupportsPaletteConversion = true;
  info.bSupportsClipControl = true;
  info.bSupportsDepthClamp = true;
  info.bSupportsReversedDepthRange = false;
  info.bSupportsMultithreading = false;
  info.bSupportsCopyToVram = true;
  info.bSupportsLargePoints = false;
  info.bSupportsDepthReadback = true;
  info.bSupportsPartialDepthCopies = false;
  info.bSupportsBitfield = false;
  info.bSupportsDynamicSamplerIndexing = false;
  info.bSupportsFramebufferFetch = false;
  info.bSupportsBackgroundCompiling = true;
  info.bSupportsShaderBinaries = true;
  info.bSupportsPipelineCacheData = false;
  info.bSupportsCoarseDerivatives = true;
  info.bSupportsTextureQueryLevels = true;
  info.bSupportsLodBiasInSampler = true;
  info.bSupportsSettingObjectNames = true;
  info.bSupportsPartialMultisampleResolve = true;
  info.bSupportsDynamicVertexLoader = false;
  info.bSupportsHDROutput = true;

  // Assumed present until a device says otherwise (below).
  info.bSupportsGPUTextureDecoding = true;
  info.bSupportsST3CTextures = true;
  info.bSupportsBPTCTextures = true;
  info.bSupportsEarlyZ = true;
  info.bSupportsBBox = true;
  info.bSupportsFragmentStoresAndAtomics = true;
  info.bSupportsGSInstancing = true;
  info.bSupportsSSAA = true;

  // These three may create a temporary device.
  info.bSupportsLogicOp = D3D::SupportsLogicOp(static_cast<u32>(g_Config.iAdapter));
  info.Adapters = D3DCommon::GetAdapterNames();
  info.AAModes = D3D::GetAAModes(static_cast<u32>(g_Config.iAdapter));

  // With a real device (i.e. while booting) the optional features are checked.
  if (!D3D::device)
    return;

  info.bSupportsST3CTextures = D3D::SupportsTextureFormat(DXGI_FORMAT_BC1_UNORM) &&
                               D3D::SupportsTextureFormat(DXGI_FORMAT_BC2_UNORM) &&
                               D3D::SupportsTextureFormat(DXGI_FORMAT_BC3_UNORM);
  info.bSupportsBPTCTextures = D3D::SupportsTextureFormat(DXGI_FORMAT_BC7_UNORM);

  // Shader model 5 (feature level 11_0) features.
  const bool sm5 = D3D::feature_level >= D3D_FEATURE_LEVEL_11_0;
  info.bSupportsEarlyZ = sm5;
  info.bSupportsBBox = sm5;
  info.bSupportsFragmentStoresAndAtomics = sm5;
  info.bSupportsGSInstancing = sm5;
  info.bSupportsSSAA = sm5;
  info.bSupportsGPUTextureDecoding = sm5;
}

bool VideoBackend::Initialize(const WindowSystemInfo& wsi)
{
  if (!D3D::Create(static_cast<u32>(g_Config.iAdapter), g_Config.bEnableValidationLayer))
    return false;

  FillBackendInfo();
  UpdateActiveConfig();

  // Without a window (headless) there is no swap chain.
  std::unique_ptr<SwapChain> swap_chain;
  if (wsi.render_surface)
  {
    swap_chain = SwapChain::Create(wsi);
    if (!swap_chain)
    {
      PanicAlertFmtT("Failed to create D3D swap chain");
      ShutdownShared();
      D3D::Destroy();
      return false;
    }
  }

  auto gfx = std::make_unique<DX11::Gfx>(std::move(swap_chain), wsi.render_surface_scale);
  auto vertex_manager = std::make_unique<VertexManager>();
  auto perf_query = std::make_unique<PerfQuery>();
  auto bounding_box = std::make_unique<D3DBoundingBox>();
  return InitializeShared(std::move(gfx), std::move(vertex_manager), std::move(perf_query),
                          std::move(bounding_box));
}

void VideoBackend::Shutdown()
{
  ShutdownShared();
  D3D::Destroy();
}
}  // namespace DX11
