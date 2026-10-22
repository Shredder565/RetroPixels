// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/OGL/VideoBackend.h"

#include <memory>
#include <string>
#include <vector>

#include "Common/Common.h"
#include "Common/GL/GLContext.h"
#include "Common/GL/GLUtil.h"
#include "Common/MsgHandler.h"

#include "Core/Config/GraphicsSettings.h"

#include "VideoBackends/OGL/OGLBoundingBox.h"
#include "VideoBackends/OGL/OGLConfig.h"
#include "VideoBackends/OGL/OGLGfx.h"
#include "VideoBackends/OGL/OGLPerfQuery.h"
#include "VideoBackends/OGL/OGLVertexManager.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"
#include "VideoBackends/OGL/SamplerCache.h"

#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
namespace
{
// Quad-buffered stereo needs a stereo pixel format, so it is decided at context creation.
std::unique_ptr<GLContext> CreateMainContext(const WindowSystemInfo& wsi)
{
  const bool stereo = g_Config.stereo_mode == StereoMode::QuadBuffer;
  return GLContext::Create(wsi, stereo, true, Config::Get(Config::GFX_PREFER_GLES));
}

// Settings that hold for every GL driver. The ones marked "assumed" are only a starting point
// for the options dialog and are replaced with what the driver really offers by
// PopulateConfig().
void SetFixedBackendInfo()
{
  auto& info = g_backend_info;
  info.api_type = APIType::OpenGL;
  info.MaxTextureSize = 16384;
  info.bUsesLowerLeftOrigin = true;
  info.bSupportsExclusiveFullscreen = false;
  info.bSupportsGeometryShaders = true;
  info.bSupportsComputeShaders = false;
  info.bSupports3DVision = false;
  info.bSupportsPostProcessing = true;
  info.bSupportsSSAA = true;
  info.bSupportsReversedDepthRange = true;
  info.bSupportsLogicOp = true;
  info.bSupportsMultithreading = false;
  info.bSupportsCopyToVram = true;
  info.bSupportsLargePoints = true;
  info.bSupportsDepthReadback = true;
  info.bSupportsPartialDepthCopies = true;
  info.bSupportsShaderBinaries = false;
  info.bSupportsPipelineCacheData = false;
  info.bSupportsLodBiasInSampler = true;
  info.bSupportsPartialMultisampleResolve = true;
  // GL has no pipeline objects for a dynamic vertex loader to save.
  info.bSupportsDynamicVertexLoader = false;

  // Offered without checking for texel buffers / SSBOs, since no context exists when the
  // options dialog asks; the option can therefore show on drivers that cannot do it.
  info.bSupportsGPUTextureDecoding = true;
  info.bSupportsBBox = true;

  // Assumed.
  info.bSupportsDualSourceBlend = true;
  info.bSupportsPrimitiveRestart = true;
  info.bSupportsPaletteConversion = true;
  info.bSupportsClipControl = true;
  info.bSupportsDepthClamp = true;
  info.bSupportsST3CTextures = false;
  info.bSupportsBPTCTextures = false;
  info.bSupportsCoarseDerivatives = false;
  info.bSupportsTextureQueryLevels = false;
  info.bSupportsSettingObjectNames = false;

  info.bUsesExplictQuadBuffering = true;

  info.Adapters.clear();

  // 1 means no anti-aliasing, as in the D3D backends.
  info.AAModes = {1, 2, 4, 8};
}
}  // namespace

std::string VideoBackend::GetConfigName() const
{
  return CONFIG_NAME;
}

std::string VideoBackend::GetDisplayName() const
{
  return g_ogl_config.bIsES ? _trans("OpenGL ES") : _trans("OpenGL");
}

void VideoBackend::InitBackendInfo(const WindowSystemInfo& wsi)
{
  // A throwaway context, just to ask the driver what it can do.
  const std::unique_ptr<GLContext> probe = CreateMainContext(wsi);
  if (probe != nullptr)
    FillBackendInfo(probe.get());
}

bool VideoBackend::InitializeGLExtensions(GLContext* context)
{
  if (!GLExtensions::Init(context))
  {
    // Everything is drawn with shaders, which need at least GL 2.0.
    PanicAlertFmtT("GPU: OGL ERROR: Does your video card support OpenGL 2.0?");
    return false;
  }

  // Integer vertex attributes need a GL 3 entry point.
  if (GLExtensions::Version() >= 300)
    return true;
  PanicAlertFmtT("GPU: OGL ERROR: Need OpenGL version 3.\n"
                 "GPU: Does your video card support OpenGL 3?");
  return false;
}

bool VideoBackend::FillBackendInfo(GLContext* context)
{
  if (!InitializeGLExtensions(context))
    return false;

  SetFixedBackendInfo();

  GLint vertex_attribs = 0;
  glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &vertex_attribs);
  if (vertex_attribs < 16)
  {
    PanicAlertFmtT("GPU: OGL ERROR: Number of attributes {0} not enough.\n"
                   "GPU: Does your video card support OpenGL 2.x?",
                   vertex_attribs);
    return false;
  }

  GLint texture_limit = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &texture_limit);
  g_backend_info.MaxTextureSize = static_cast<u32>(texture_limit);
  if (texture_limit < 1024)
  {
    PanicAlertFmtT("GL_MAX_TEXTURE_SIZE is {0} - must be at least 1024.", texture_limit);
    return false;
  }

  // Stops here when a required extension is missing; later setup could crash without it.
  return PopulateConfig(context);
}

bool VideoBackend::Initialize(const WindowSystemInfo& wsi)
{
  std::unique_ptr<GLContext> context = CreateMainContext(wsi);
  if (context == nullptr || !FillBackendInfo(context.get()))
    return false;

  auto gfx = std::make_unique<OGLGfx>(std::move(context), wsi.render_surface_scale);
  ProgramShaderCache::Init();
  g_sampler_cache = std::make_unique<SamplerCache>();

  auto vertex_manager = std::make_unique<VertexManager>();
  auto perf_query = GetPerfQuery(gfx->IsGLES());
  auto bounding_box = std::make_unique<OGLBoundingBox>();
  return InitializeShared(std::move(gfx), std::move(vertex_manager), std::move(perf_query),
                          std::move(bounding_box));
}

void VideoBackend::Shutdown()
{
  ShutdownShared();
  ProgramShaderCache::Shutdown();
  g_sampler_cache.reset();
}
}  // namespace OGL
