// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Null/VideoBackend.h"

#include <memory>

#include "Common/Common.h"

#include "VideoBackends/Null/NullBoundingBox.h"
#include "VideoBackends/Null/NullGfx.h"
#include "VideoBackends/Null/NullVertexManager.h"
#include "VideoBackends/Null/PerfQuery.h"
#include "VideoBackends/Null/TextureCache.h"

#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace Null
{
std::string VideoBackend::GetDisplayName() const
{
  // i18n: Null is referring to the null video backend, which renders nothing
  return _trans("Null");
}

void VideoBackend::InitBackendInfo(const WindowSystemInfo&)
{
  BackendInfo& info = g_backend_info;
  info.api_type = APIType::Nothing;
  info.MaxTextureSize = 16384;
  info.Adapters.clear();
  info.AAModes = {1};  // a single sample: no MSAA

  // Claim the features that make VideoCommon take its most general paths, since nothing is
  // drawn either way.
  for (bool* claimed : {
           &info.bSupportsExclusiveFullscreen,
           &info.bSupportsDualSourceBlend,
           &info.bSupportsPrimitiveRestart,
           &info.bSupportsGeometryShaders,
           &info.bSupportsEarlyZ,
           &info.bSupportsBindingLayout,
           &info.bSupportsBBox,
           &info.bSupportsGSInstancing,
           &info.bSupportsPaletteConversion,
           &info.bSupportsClipControl,
           &info.bSupportsSSAA,
           &info.bSupportsDepthClamp,
           &info.bSupportsReversedDepthRange,
           &info.bSupportsPartialMultisampleResolve,
       })
  {
    *claimed = true;
  }

  for (bool* unsupported : {
           &info.bSupportsComputeShaders,      &info.bSupports3DVision,
           &info.bSupportsPostProcessing,      &info.bSupportsMultithreading,
           &info.bSupportsGPUTextureDecoding,  &info.bSupportsST3CTextures,
           &info.bSupportsBPTCTextures,        &info.bSupportsFramebufferFetch,
           &info.bSupportsBackgroundCompiling, &info.bSupportsLogicOp,
           &info.bSupportsLargePoints,         &info.bSupportsDepthReadback,
           &info.bSupportsPartialDepthCopies,  &info.bSupportsShaderBinaries,
           &info.bSupportsPipelineCacheData,   &info.bSupportsCoarseDerivatives,
           &info.bSupportsTextureQueryLevels,  &info.bSupportsLodBiasInSampler,
           &info.bSupportsSettingObjectNames,  &info.bSupportsDynamicVertexLoader,
       })
  {
    *unsupported = false;
  }
}

bool VideoBackend::Initialize(const WindowSystemInfo&)
{
  return InitializeShared(std::make_unique<NullGfx>(), std::make_unique<VertexManager>(),
                          std::make_unique<PerfQuery>(), std::make_unique<NullBoundingBox>(),
                          std::make_unique<NullEFBInterface>(), std::make_unique<TextureCache>());
}

void VideoBackend::Shutdown()
{
  ShutdownShared();
}
}  // namespace Null
