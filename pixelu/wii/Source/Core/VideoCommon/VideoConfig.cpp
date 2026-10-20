// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/VideoConfig.h"

#include <algorithm>
#include <optional>

#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/Contains.h"

#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Movie.h"
#include "Core/System.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/FreeLookCamera.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/XFMemory.h"

VideoConfig g_Config;
VideoConfig g_ActiveConfig;
BackendInfo g_backend_info;

namespace
{
std::optional<CPUThreadConfigCallback::ConfigChangedCallbackID> s_cpu_config_callback;
Common::EventHook s_after_frame_hook;

template <typename T, typename U>
void Load(T& out, const Config::Info<U>& info)
{
  out = Config::Get(info);
}

// VSync only applies at normal speed; fast-forwarding (or the temporary unthrottle hotkey)
// turns it off.
bool VSyncWanted(bool vsync_setting)
{
  if (!vsync_setting || Core::GetIsThrottlerTempDisabled())
    return false;
  return Config::Get(Config::MAIN_EMULATION_SPEED) == 1.0;
}

// Runs on the CPU thread when any setting changes. The GPU thread is held while g_Config is
// reread and corrected, so it never sees a half-updated or not yet validated config; it picks
// up the change at the next frame.
void ReloadConfigOnCpuThread()
{
  auto& system = Core::System::GetInstance();
  const bool hold_gpu = Core::IsRunning(system);
  if (hold_gpu)
    system.GetFifo().PauseAndLock();

  g_Config.Refresh();
  g_Config.VerifyValidity();

  if (hold_gpu)
    system.GetFifo().RestoreState(true);
}

u32 AutoCompilerThreads()
{
  // Leave room for the CPU, GPU and UI threads; between 1 and 4 workers.
  return static_cast<u32>(std::clamp(cpu_info.num_cores - 3, 1, 4));
}

u32 AutoPrecompilerThreads()
{
  // Precompiling before the game starts can use everything but two cores (UI and system).
  return static_cast<u32>(std::max(cpu_info.num_cores - 2, 1));
}

// The settings whose change has to be passed on, captured before g_ActiveConfig is refreshed.
struct WatchedSettings
{
  explicit WatchedSettings(const VideoConfig& c)
      : host_config(ShaderHostConfig::GetCurrent()), stereo(c.stereo_mode),
        multisamples(c.iMultisamples), anisotropy(c.iMaxAnisotropy),
        efb_access_tile_size(c.iEFBAccessTileSize), texture_filtering(c.texture_filtering_mode),
        vsync_active(c.bVSyncActive), bbox(c.bBBoxEnable), efb_scale(c.iEFBScale),
        mod_change_count(c.graphics_mod_config ? c.graphics_mod_config->GetChangeCount() : 0),
        mods_enabled(c.bGraphicMods), aspect(c.aspect_mode),
        suggested_aspect(c.suggested_aspect_mode), widescreen_hack(c.bWidescreenHack),
        post_processing_shader(c.sPostProcessingShader), hdr(c.bHDR)
  {
  }

  ShaderHostConfig host_config;
  StereoMode stereo;
  u32 multisamples;
  AnisotropicFilteringMode anisotropy;
  int efb_access_tile_size;
  TextureFilteringMode texture_filtering;
  bool vsync_active;
  bool bbox;
  int efb_scale;
  u32 mod_change_count;
  bool mods_enabled;
  AspectMode aspect;
  AspectMode suggested_aspect;
  bool widescreen_hack;
  std::string post_processing_shader;
  bool hdr;
};

u32 ChangedBits(const WatchedSettings& old, const ShaderHostConfig& host, const VideoConfig& c)
{
  u32 bits = 0;
  const auto flag = [&bits](bool changed, u32 bit) {
    if (changed)
      bits |= bit;
  };
  flag(old.host_config.bits != host.bits, CONFIG_CHANGE_BIT_HOST_CONFIG);
  flag(old.stereo != c.stereo_mode, CONFIG_CHANGE_BIT_STEREO_MODE);
  flag(old.multisamples != c.iMultisamples, CONFIG_CHANGE_BIT_MULTISAMPLES);
  flag(old.anisotropy != c.iMaxAnisotropy, CONFIG_CHANGE_BIT_ANISOTROPY);
  flag(old.texture_filtering != c.texture_filtering_mode,
       CONFIG_CHANGE_BIT_FORCE_TEXTURE_FILTERING);
  flag(old.vsync_active != c.bVSyncActive, CONFIG_CHANGE_BIT_VSYNC);
  flag(old.bbox != c.bBBoxEnable, CONFIG_CHANGE_BIT_BBOX);
  flag(old.efb_scale != c.iEFBScale, CONFIG_CHANGE_BIT_TARGET_SIZE);
  flag(old.aspect != c.aspect_mode || old.suggested_aspect != c.suggested_aspect_mode ||
           old.widescreen_hack != c.bWidescreenHack,
       CONFIG_CHANGE_BIT_ASPECT_RATIO);
  flag(old.post_processing_shader != c.sPostProcessingShader,
       CONFIG_CHANGE_BIT_POST_PROCESSING_SHADER);
  flag(old.hdr != c.bHDR, CONFIG_CHANGE_BIT_HDR);
  return bits;
}
}  // namespace

void UpdateActiveConfig()
{
  g_ActiveConfig = g_Config;
  g_ActiveConfig.bVSyncActive = VSyncWanted(g_ActiveConfig.bVSync);
}

void VideoConfig::Refresh()
{
  if (!s_cpu_config_callback)
    s_cpu_config_callback = CPUThreadConfigCallback::AddConfigChangedCallback(ReloadConfigOnCpuThread);

  // General
  Load(bVSync, Config::GFX_VSYNC);
  Load(bWidescreenHack, Config::GFX_WIDESCREEN_HACK);
  Load(aspect_mode, Config::GFX_ASPECT_RATIO);
  Load(custom_aspect_width, Config::GFX_CUSTOM_ASPECT_RATIO_WIDTH);
  Load(custom_aspect_height, Config::GFX_CUSTOM_ASPECT_RATIO_HEIGHT);
  Load(suggested_aspect_mode, Config::GFX_SUGGESTED_ASPECT_RATIO);
  Load(widescreen_heuristic_transition_threshold,
       Config::GFX_WIDESCREEN_HEURISTIC_TRANSITION_THRESHOLD);
  Load(widescreen_heuristic_aspect_ratio_slop, Config::GFX_WIDESCREEN_HEURISTIC_ASPECT_RATIO_SLOP);
  Load(widescreen_heuristic_standard_ratio, Config::GFX_WIDESCREEN_HEURISTIC_STANDARD_RATIO);
  Load(widescreen_heuristic_widescreen_ratio, Config::GFX_WIDESCREEN_HEURISTIC_WIDESCREEN_RATIO);
  Load(bCropToAspectRatio, Config::GFX_CROP_TO_ASPECT_RATIO);
  Load(bCropCustom, Config::GFX_CROP_CUSTOM);
  Load(iCropCustomLeft, Config::GFX_CROP_CUSTOM_LEFT);
  Load(iCropCustomTop, Config::GFX_CROP_CUSTOM_TOP);
  Load(iCropCustomRight, Config::GFX_CROP_CUSTOM_RIGHT);
  Load(iCropCustomBottom, Config::GFX_CROP_CUSTOM_BOTTOM);
  Load(bShaderCache, Config::GFX_SHADER_CACHE);

  // Enhancements
  Load(iMultisamples, Config::GFX_MSAA);
  Load(bSSAA, Config::GFX_SSAA);
  Load(iEFBScale, Config::GFX_EFB_SCALE);
  Load(texture_filtering_mode, Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING);
  Load(output_resampling_mode, Config::GFX_ENHANCE_OUTPUT_RESAMPLING);
  Load(iMaxAnisotropy, Config::GFX_ENHANCE_MAX_ANISOTROPY);
  Load(sPostProcessingShader, Config::GFX_ENHANCE_POST_SHADER);
  Load(bForceTrueColor, Config::GFX_ENHANCE_FORCE_TRUE_COLOR);
  Load(bDisableCopyFilter, Config::GFX_ENHANCE_DISABLE_COPY_FILTER);
  Load(bArbitraryMipmapDetection, Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION);
  Load(fArbitraryMipmapDetectionThreshold,
       Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD);
  Load(bHDR, Config::GFX_ENHANCE_HDR_OUTPUT);

  auto& cc = color_correction;
  Load(cc.bCorrectColorSpace, Config::GFX_CC_CORRECT_COLOR_SPACE);
  Load(cc.game_color_space, Config::GFX_CC_GAME_COLOR_SPACE);
  Load(cc.bCorrectGamma, Config::GFX_CC_CORRECT_GAMMA);
  Load(cc.fGameGamma, Config::GFX_CC_GAME_GAMMA);
  Load(cc.bSDRDisplayGammaSRGB, Config::GFX_CC_SDR_DISPLAY_GAMMA_SRGB);
  Load(cc.fSDRDisplayCustomGamma, Config::GFX_CC_SDR_DISPLAY_CUSTOM_GAMMA);
  Load(cc.fHDRPaperWhiteNits, Config::GFX_CC_HDR_PAPER_WHITE_NITS);

  // Information
  Load(bShowFPS, Config::GFX_SHOW_FPS);
  Load(bShowFTimes, Config::GFX_SHOW_FTIMES);
  Load(bShowVPS, Config::GFX_SHOW_VPS);
  Load(bShowVTimes, Config::GFX_SHOW_VTIMES);
  Load(bShowGraphs, Config::GFX_SHOW_GRAPHS);
  Load(bShowSpeed, Config::GFX_SHOW_SPEED);
  Load(bShowSpeedColors, Config::GFX_SHOW_SPEED_COLORS);
  iPerfSampleUSec = Config::Get(Config::GFX_PERF_SAMP_WINDOW) * 1000;  // setting is in ms
  Load(bOverlayStats, Config::GFX_OVERLAY_STATS);
  Load(bOverlayProjStats, Config::GFX_OVERLAY_PROJ_STATS);
  Load(bOverlayScissorStats, Config::GFX_OVERLAY_SCISSOR_STATS);
  Load(bShowInternalResolution, Config::GFX_SHOW_INTERNAL_RESOLUTION);
  Load(bTexFmtOverlayEnable, Config::GFX_TEXFMT_OVERLAY_ENABLE);
  Load(bTexFmtOverlayCenter, Config::GFX_TEXFMT_OVERLAY_CENTER);
  Load(bLogRenderTimeToFile, Config::GFX_LOG_RENDER_TIME_TO_FILE);

  // Render
  Load(bWireFrame, Config::GFX_ENABLE_WIREFRAME);
  Load(bDisableFog, Config::GFX_DISABLE_FOG);

  // Utility
  Load(bDumpTextures, Config::GFX_DUMP_TEXTURES);
  Load(bDumpMipmapTextures, Config::GFX_DUMP_MIP_TEXTURES);
  Load(bDumpBaseTextures, Config::GFX_DUMP_BASE_TEXTURES);
  Load(bHiresTextures, Config::GFX_HIRES_TEXTURES);
  Load(bCacheHiresTextures, Config::GFX_CACHE_HIRES_TEXTURES);
  Load(bDumpEFBTarget, Config::GFX_DUMP_EFB_TARGET);
  Load(bDumpXFBTarget, Config::GFX_DUMP_XFB_TARGET);
  Load(bBorderlessFullscreen, Config::GFX_BORDERLESS_FULLSCREEN);
  Load(bEnableGPUTextureDecoding, Config::GFX_ENABLE_GPU_TEXTURE_DECODING);
  Load(bPreferVSForLinePointExpansion, Config::GFX_PREFER_VS_FOR_LINE_POINT_EXPANSION);
  Load(bGraphicMods, Config::GFX_MODS_ENABLE);

  // Hacks
  Load(bEFBAccessEnable, Config::GFX_HACK_EFB_ACCESS_ENABLE);
  Load(bEFBAccessDeferInvalidation, Config::GFX_HACK_EFB_DEFER_INVALIDATION);
  Load(bPerfQueriesEnable, Config::GFX_PERF_QUERIES_ENABLE);
  Load(bBBoxEnable, Config::GFX_HACK_BBOX_ENABLE);
  Load(bCPUCull, Config::GFX_CPU_CULL);
  Load(bEFBEmulateFormatChanges, Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES);
  Load(bSkipEFBCopyToRam, Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM);
  Load(bSkipXFBCopyToRam, Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM);
  Load(bDisableCopyToVRAM, Config::GFX_HACK_DISABLE_COPY_TO_VRAM);
  Load(bDeferEFBCopies, Config::GFX_HACK_DEFER_EFB_COPIES);
  Load(bImmediateXFB, Config::GFX_HACK_IMMEDIATE_XFB);
  Load(bVISkip, Config::GFX_HACK_VI_SKIP);
  // Skipping VIs implies skipping duplicate XFBs.
  bSkipPresentingDuplicateXFBs = bVISkip || Config::Get(Config::GFX_HACK_SKIP_DUPLICATE_XFBS);
  Load(bCopyEFBScaled, Config::GFX_HACK_COPY_EFB_SCALED);
  Load(iSafeTextureCache_ColorSamples, Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES);
  Load(bEnablePixelLighting, Config::GFX_ENABLE_PIXEL_LIGHTING);
  Load(bFastDepthCalc, Config::GFX_FAST_DEPTH_CALC);
  Load(bVertexRounding, Config::GFX_HACK_VERTEX_ROUNDING);
  Load(iEFBAccessTileSize, Config::GFX_HACK_EFB_ACCESS_TILE_SIZE);
  Load(iMissingColorValue, Config::GFX_HACK_MISSING_COLOR_VALUE);
  Load(bFastTextureSampling, Config::GFX_HACK_FAST_TEXTURE_SAMPLING);
#ifdef __APPLE__
  Load(bNoMipmapping, Config::GFX_HACK_NO_MIPMAPPING);
#endif

  // Stereoscopy: depth and convergence are the base value times a percentage, scaled to the
  // units the geometry shader uses.
  Load(stereo_mode, Config::GFX_STEREO_MODE);
  Load(stereo_per_eye_resolution_full, Config::GFX_STEREO_PER_EYE_RESOLUTION_FULL);
  const float depth = Config::Get(Config::GFX_STEREO_DEPTH);
  const float depth_percent = Config::Get(Config::GFX_STEREO_DEPTH_PERCENTAGE);
  stereo_depth = depth * depth_percent * 0.00001f;
  const float convergence = Config::Get(Config::GFX_STEREO_CONVERGENCE);
  const float convergence_percent = Config::Get(Config::GFX_STEREO_CONVERGENCE_PERCENTAGE);
  stereo_convergence = convergence * convergence_percent * 0.01f;
  Load(bStereoSwapEyes, Config::GFX_STEREO_SWAP_EYES);
  Load(bStereoEFBMonoDepth, Config::GFX_STEREO_EFB_MONO_DEPTH);

  // Backend specific
  Load(iAdapter, Config::GFX_ADAPTER);
  Load(iManuallyUploadBuffers, Config::GFX_MTL_MANUALLY_UPLOAD_BUFFERS);
  Load(iUsePresentDrawable, Config::GFX_MTL_USE_PRESENT_DRAWABLE);
  Load(bEnableValidationLayer, Config::GFX_ENABLE_VALIDATION_LAYER);
  Load(bBackendMultithreading, Config::GFX_BACKEND_MULTITHREADING);
  Load(iCommandBufferExecuteInterval, Config::GFX_COMMAND_BUFFER_EXECUTE_INTERVAL);

  // Shader compilation
  Load(bWaitForShadersBeforeStarting, Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING);
  Load(iShaderCompilationMode, Config::GFX_SHADER_COMPILATION_MODE);
  Load(iShaderCompilerThreads, Config::GFX_SHADER_COMPILER_THREADS);
  Load(iShaderPrecompilerThreads, Config::GFX_SHADER_PRECOMPILER_THREADS);

  Load(customDriverLibraryName, Config::GFX_DRIVER_LIB_NAME);
  Load(vertex_loader_type, Config::GFX_VERTEX_LOADER_TYPE);
}

void VideoConfig::VerifyValidity()
{
  const int adapter_count = static_cast<int>(g_backend_info.Adapters.size());
  if (iAdapter < 0 || iAdapter >= adapter_count)
    iAdapter = 0;

  if (!Common::Contains(g_backend_info.AAModes, iMultisamples))
    iMultisamples = 1;

  if (stereo_mode != StereoMode::Off && !g_backend_info.bSupportsGeometryShaders)
  {
    OSD::AddMessage(
        "Stereoscopic 3D isn't supported by your GPU, support for OpenGL 3.2 is required.",
        10000);
    stereo_mode = StereoMode::Off;
  }
}

void VideoConfig::Init()
{
  s_after_frame_hook =
      GetVideoEvents().after_frame_event.Register([](Core::System&) { CheckForConfigChanges(); });
}

void VideoConfig::Shutdown()
{
  s_after_frame_hook.reset();

  if (s_cpu_config_callback)
  {
    CPUThreadConfigCallback::RemoveConfigChangedCallback(*s_cpu_config_callback);
    s_cpu_config_callback.reset();
  }
}

bool VideoConfig::UsingUberShaders() const
{
  switch (iShaderCompilationMode)
  {
  case ShaderCompilationMode::SynchronousUberShaders:
  case ShaderCompilationMode::AsynchronousUberShaders:
    return true;
  default:
    return false;
  }
}

u32 VideoConfig::GetShaderCompilerThreads() const
{
  if (!g_backend_info.bSupportsBackgroundCompiling)
    return 0;
  return iShaderCompilerThreads < 0 ? AutoCompilerThreads() :
                                      static_cast<u32>(iShaderCompilerThreads);
}

u32 VideoConfig::GetShaderPrecompilerThreads() const
{
  // Without a wait at start-up, precompiling happens in the background with the normal pool.
  if (!bWaitForShadersBeforeStarting)
    return GetShaderCompilerThreads();
  if (!g_backend_info.bSupportsBackgroundCompiling)
    return 0;
  if (iShaderPrecompilerThreads >= 0)
    return static_cast<u32>(iShaderPrecompilerThreads);
  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_MULTITHREADED_SHADER_PRECOMPILATION))
    return 1;
  return AutoPrecompilerThreads();
}

void CheckForConfigChanges()
{
  const WatchedSettings before(g_ActiveConfig);

  UpdateActiveConfig();
  g_vertex_manager->OnConfigChange();
  g_freelook_camera.RefreshConfig();

  // Graphics mods: load the game's mod group when mods get turned on, and reapply it when it
  // was edited.
  if (g_ActiveConfig.bGraphicMods && !before.mods_enabled)
  {
    g_ActiveConfig.graphics_mod_config = GraphicsModGroupConfig(SConfig::GetInstance().GetGameID());
    g_ActiveConfig.graphics_mod_config->Load();
  }
  if (g_ActiveConfig.graphics_mod_config &&
      g_ActiveConfig.graphics_mod_config->GetChangeCount() != before.mod_change_count)
  {
    g_graphics_mod_manager->Load(*g_ActiveConfig.graphics_mod_config);
  }

  g_texture_cache->OnConfigChanged(g_ActiveConfig);

  // The EFB peek tile size is handled here; backends are not told about it.
  if (g_ActiveConfig.iEFBAccessTileSize != before.efb_access_tile_size)
    g_framebuffer_manager->SetEFBCacheTileSize(
        static_cast<u32>(std::max(g_ActiveConfig.iEFBAccessTileSize, 0)));

  const ShaderHostConfig host_config = ShaderHostConfig::GetCurrent();
  const u32 changed_bits = ChangedBits(before, host_config, g_ActiveConfig);
  if (changed_bits == 0)
    return;

  const float scale_before = static_cast<float>(g_framebuffer_manager->GetEFBScale());
  constexpr u32 efb_layout_bits = CONFIG_CHANGE_BIT_MULTISAMPLES | CONFIG_CHANGE_BIT_STEREO_MODE |
                                  CONFIG_CHANGE_BIT_TARGET_SIZE | CONFIG_CHANGE_BIT_HDR;
  if ((changed_bits & efb_layout_bits) != 0)
    g_framebuffer_manager->RecreateEFBFramebuffer(g_ActiveConfig.iEFBScale);
  if (static_cast<float>(g_framebuffer_manager->GetEFBScale()) != scale_before)
    Core::System::GetInstance().GetPixelShaderManager().Dirty();

  constexpr u32 shader_bits = CONFIG_CHANGE_BIT_HOST_CONFIG | CONFIG_CHANGE_BIT_MULTISAMPLES;
  if ((changed_bits & shader_bits) != 0)
  {
    OSD::AddMessage("Video config changed, reloading shaders.", OSD::Duration::NORMAL);
    g_gfx->WaitForGPUIdle();
    g_vertex_manager->InvalidatePipelineObject();
    g_vertex_manager->NotifyCustomShaderCacheOfHostChange(host_config);
    g_shader_cache->SetHostConfig(host_config);
    g_shader_cache->Reload();
    g_framebuffer_manager->RecompileShaders();
  }

  // A new internal resolution scales viewport and scissor differently.
  if ((changed_bits & CONFIG_CHANGE_BIT_TARGET_SIZE) != 0)
  {
    BPFunctions::SetScissorAndViewport(g_framebuffer_manager.get(), bpmem.scissorTL,
                                       bpmem.scissorBR, bpmem.scissorOffset, xfmem.viewport);
  }

  GetVideoEvents().config_changed_event.Trigger(changed_bits);
}
