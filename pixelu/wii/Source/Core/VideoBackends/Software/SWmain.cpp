// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Software/VideoBackend.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "VideoBackends/Software/Clipper.h"
#include "VideoBackends/Software/Rasterizer.h"
#include "VideoBackends/Software/SWBoundingBox.h"
#include "VideoBackends/Software/SWEfbInterface.h"
#include "VideoBackends/Software/SWGfx.h"
#include "VideoBackends/Software/SWOGLWindow.h"
#include "VideoBackends/Software/SWVertexLoader.h"
#include "VideoBackends/Software/TextureCache.h"

#include "VideoCommon/PerfQueryBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace SW
{
namespace
{
// Performance queries are answered from the counters the EFB keeps as pixels pass through it,
// so results are always ready.
class EfbCounterQuery final : public PerfQueryBase
{
public:
  void EnableQuery(PerfQueryGroup) override {}
  void DisableQuery(PerfQueryGroup) override {}
  void ResetQuery() override { EfbInterface::ResetPerfQuery(); }
  u32 GetQueryResult(PerfQueryType type) override { return EfbInterface::GetPerfQueryResult(type); }
  void FlushResults() override {}
  bool IsFlushed() const override { return true; }
};
}  // namespace

void VideoSoftware::InitBackendInfo(const WindowSystemInfo&)
{
  BackendInfo& info = g_backend_info;
  info.api_type = APIType::Nothing;
  info.MaxTextureSize = 16384;
  info.AAModes = {1};  // no multisampling

  // What the software pipeline emulates itself.
  info.bSupportsDualSourceBlend = true;
  info.bSupportsEarlyZ = true;
  info.bSupportsLogicOp = true;
  info.bSupportsBBox = true;
  info.bSupportsPartialMultisampleResolve = true;

  // GPU-side features that have no meaning here.
  for (bool* unsupported : {
           &info.bUsesLowerLeftOrigin,         &info.bSupports3DVision,
           &info.bSupportsPrimitiveRestart,    &info.bSupportsMultithreading,
           &info.bSupportsComputeShaders,      &info.bSupportsGPUTextureDecoding,
           &info.bSupportsST3CTextures,        &info.bSupportsBPTCTextures,
           &info.bSupportsCopyToVram,          &info.bSupportsLargePoints,
           &info.bSupportsDepthReadback,       &info.bSupportsPartialDepthCopies,
           &info.bSupportsFramebufferFetch,    &info.bSupportsBackgroundCompiling,
           &info.bSupportsShaderBinaries,      &info.bSupportsPipelineCacheData,
           &info.bSupportsCoarseDerivatives,   &info.bSupportsTextureQueryLevels,
           &info.bSupportsLodBiasInSampler,    &info.bSupportsSettingObjectNames,
           &info.bSupportsDynamicVertexLoader,
       })
  {
    *unsupported = false;
  }
}

bool VideoSoftware::Initialize(const WindowSystemInfo& wsi)
{
  auto window = SWOGLWindow::Create(wsi);
  if (window == nullptr)
    return false;

  Clipper::Init();
  Rasterizer::Init();

  auto gfx = std::make_unique<SWGfx>(std::move(window));
  return InitializeShared(std::move(gfx), std::make_unique<SWVertexLoader>(),
                          std::make_unique<EfbCounterQuery>(), std::make_unique<SWBoundingBox>(),
                          std::make_unique<SWEFBInterface>(), std::make_unique<TextureCache>());
}

void VideoSoftware::Shutdown()
{
  ShutdownShared();
}

std::string VideoSoftware::GetDisplayName() const
{
  return _trans("Software Renderer");
}

std::string VideoSoftware::GetConfigName() const
{
  return CONFIG_NAME;
}

std::optional<std::string> VideoSoftware::GetWarningMessage() const
{
  return _trans("The software renderer is significantly slower than other "
                "backends and is only recommended for debugging purposes.\n\nDo you "
                "really want to enable software rendering? If unsure, select 'No'.");
}

}  // namespace SW
