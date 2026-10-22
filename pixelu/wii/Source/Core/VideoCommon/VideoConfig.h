// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Two copies of the video settings exist. The UI and config system write g_Config; the video
// code reads g_ActiveConfig, which is refreshed from g_Config once per frame so settings never
// change half-way through a frame. Nothing should write g_ActiveConfig directly.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsModGroup.h"
#include "VideoCommon/VideoCommon.h"

constexpr int EFB_SCALE_AUTO_INTEGRAL = 0;

enum class AspectMode : int
{
  Auto,           // detected 4:3 or 16:9
  ForceWide,      // 16:9
  ForceStandard,  // 4:3
  Stretch,        // fill the window
  Custom,         // custom ratio, scaled like the game's own
  CustomStretch,  // custom ratio, absolute
  Raw,            // square pixels
};

enum class StereoMode : int
{
  Off,
  SideBySide,
  TopAndBottom,
  Anaglyph,
  QuadBuffer,
  Passive
};

enum class ShaderCompilationMode : int
{
  Synchronous,
  SynchronousUberShaders,
  AsynchronousUberShaders,
  AsynchronousSkipRendering
};

enum class TextureFilteringMode : int
{
  Default,
  Nearest,
  Linear,
};

enum class AnisotropicFilteringMode : int
{
  Default = -1,
  Force1x = 0,
  Force2x = 1,
  Force4x = 2,
  Force8x = 3,
  Force16x = 4,
};

enum class OutputResamplingMode : int
{
  Default,
  Bilinear,
  BSpline,
  MitchellNetravali,
  CatmullRom,
  SharpBilinear,
  AreaSampling,
};

enum class ColorCorrectionRegion : int
{
  SMPTE_NTSCM,
  SYSTEMJ_NTSCJ,
  EBU_PAL,
};

enum class TriState : int
{
  Off,
  On,
  Auto
};

enum class FrameDumpResolutionType : int
{
  // The window's drawn area (black borders excluded).
  WindowResolution,
  // The XFB at internal resolution, stretched to the game's aspect ratio.
  XFBAspectRatioCorrectedResolution,
  // The XFB at internal resolution, as stored.
  XFBRawResolution,
};

enum class VertexLoaderType : int
{
  Native,
  Software,
  Compare
};

// Flags passed with the config-changed event, one per kind of change the backends care about.
enum ConfigChangeBits : u32
{
  CONFIG_CHANGE_BIT_HOST_CONFIG = (1 << 0),
  CONFIG_CHANGE_BIT_MULTISAMPLES = (1 << 1),
  CONFIG_CHANGE_BIT_STEREO_MODE = (1 << 2),
  CONFIG_CHANGE_BIT_TARGET_SIZE = (1 << 3),
  CONFIG_CHANGE_BIT_ANISOTROPY = (1 << 4),
  CONFIG_CHANGE_BIT_FORCE_TEXTURE_FILTERING = (1 << 5),
  CONFIG_CHANGE_BIT_VSYNC = (1 << 6),
  CONFIG_CHANGE_BIT_BBOX = (1 << 7),
  CONFIG_CHANGE_BIT_ASPECT_RATIO = (1 << 8),
  CONFIG_CHANGE_BIT_POST_PROCESSING_SHADER = (1 << 9),
  CONFIG_CHANGE_BIT_HDR = (1 << 10),
};

// What the active backend and device can do. Filled by the backend's InitBackendInfo.
struct BackendInfo
{
  APIType api_type = APIType::Nothing;
  std::string DisplayName;

  std::vector<std::string> Adapters;  // D3D/Vulkan adapter names
  std::vector<u32> AAModes;

  std::string AdapterName;  // OpenGL renderer name

  u32 MaxTextureSize = 16384;
  bool bUsesLowerLeftOrigin = false;
  bool bUsesExplictQuadBuffering = false;
  bool bSupportsExclusiveFullscreen = false;  // may change at run time on Vulkan
  bool bSupportsDualSourceBlend = false;
  bool bSupportsPrimitiveRestart = false;
  bool bSupportsGeometryShaders = false;
  bool bSupportsComputeShaders = false;
  bool bSupports3DVision = false;
  bool bSupportsEarlyZ = false;
  bool bSupportsBindingLayout = false;
  bool bSupportsBBox = false;
  bool bSupportsGSInstancing = false;
  bool bSupportsPostProcessing = false;
  bool bSupportsPaletteConversion = false;
  bool bSupportsClipControl = false;
  bool bSupportsSSAA = false;
  bool bSupportsFragmentStoresAndAtomics = false;  // SSBOs / UAVs in fragment shaders
  bool bSupportsDepthClamp = false;
  bool bSupportsReversedDepthRange = false;
  bool bSupportsLogicOp = false;
  bool bSupportsMultithreading = false;
  bool bSupportsGPUTextureDecoding = false;
  bool bSupportsST3CTextures = false;
  bool bSupportsCopyToVram = false;
  bool bSupportsBitfield = false;
  bool bSupportsDynamicSamplerIndexing = false;
  bool bSupportsBPTCTextures = false;
  bool bSupportsFramebufferFetch = false;  // stands in for dual-source blending where missing
  bool bSupportsBackgroundCompiling = false;
  bool bSupportsLargePoints = false;
  bool bSupportsPartialDepthCopies = false;
  bool bSupportsDepthReadback = false;
  bool bSupportsShaderBinaries = false;
  bool bSupportsPipelineCacheData = false;
  bool bSupportsCoarseDerivatives = false;
  bool bSupportsTextureQueryLevels = false;
  bool bSupportsLodBiasInSampler = false;
  bool bSupportsSettingObjectNames = false;
  bool bSupportsPartialMultisampleResolve = false;
  bool bSupportsDynamicVertexLoader = false;
  bool bSupportsVSLinePointExpand = false;
  bool bSupportsGLLayerInFS = true;
  bool bSupportsHDROutput = false;
  bool bSupportsUnrestrictedDepthRange = false;
};

extern BackendInfo g_backend_info;

// The video settings. Final on purpose: it is copied by value every frame.
struct VideoConfig final
{
  VideoConfig() = default;
  // Reads every setting from the config system.
  void Refresh();
  // Fixes settings the current backend cannot honour.
  void VerifyValidity();
  static void Init();
  static void Shutdown();

  // General
  bool bVSync = false;
  bool bVSyncActive = false;
  bool bWidescreenHack = false;
  AspectMode aspect_mode{};
  int custom_aspect_width = 1;
  int custom_aspect_height = 1;
  AspectMode suggested_aspect_mode{};
  u32 widescreen_heuristic_transition_threshold = 0;
  float widescreen_heuristic_aspect_ratio_slop = 0.f;
  float widescreen_heuristic_standard_ratio = 0.f;
  float widescreen_heuristic_widescreen_ratio = 0.f;
  bool bCropToAspectRatio = false;
  bool bCropCustom = false;
  int iCropCustomLeft = 0;
  int iCropCustomTop = 0;
  int iCropCustomRight = 0;
  int iCropCustomBottom = 0;
  bool bShaderCache = false;

  // Enhancements
  u32 iMultisamples = 0;
  bool bSSAA = false;
  int iEFBScale = 0;
  TextureFilteringMode texture_filtering_mode = TextureFilteringMode::Default;
  OutputResamplingMode output_resampling_mode = OutputResamplingMode::Default;
  AnisotropicFilteringMode iMaxAnisotropy = AnisotropicFilteringMode::Default;
  std::string sPostProcessingShader;
  bool bForceTrueColor = false;
  bool bDisableCopyFilter = false;
  bool bArbitraryMipmapDetection = false;
  float fArbitraryMipmapDetectionThreshold = 0;
  bool bHDR = false;

  // Colour correction
  struct
  {
    bool bCorrectColorSpace = false;
    ColorCorrectionRegion game_color_space = ColorCorrectionRegion::SMPTE_NTSCM;

    bool bCorrectGamma = false;
    float fGameGamma = 2.35f;
    bool bSDRDisplayGammaSRGB = true;
    // Display gamma used when the display is not treated as sRGB.
    float fSDRDisplayCustomGamma = 2.2f;

    // Brightness of SDR white on an HDR display; 203 nits is the ITU's reference value.
    float fHDRPaperWhiteNits = 203.f;
  } color_correction;

  // Information
  bool bShowFPS = false;
  bool bShowFTimes = false;
  bool bShowVPS = false;
  bool bShowVTimes = false;
  bool bShowGraphs = false;
  bool bShowSpeed = false;
  bool bShowSpeedColors = false;
  int iPerfSampleUSec = 0;
  bool bOverlayStats = false;
  bool bOverlayProjStats = false;
  bool bOverlayScissorStats = false;
  bool bShowInternalResolution = false;
  bool bTexFmtOverlayEnable = false;
  bool bTexFmtOverlayCenter = false;
  bool bLogRenderTimeToFile = false;

  // Render
  bool bWireFrame = false;
  bool bDisableFog = false;

  // Utility
  bool bDumpTextures = false;
  bool bDumpMipmapTextures = false;
  bool bDumpBaseTextures = false;
  bool bHiresTextures = false;
  bool bCacheHiresTextures = false;
  bool bDumpEFBTarget = false;
  bool bDumpXFBTarget = false;
  bool bBorderlessFullscreen = false;
  bool bEnableGPUTextureDecoding = false;
  bool bPreferVSForLinePointExpansion = false;
  bool bGraphicMods = false;
  std::optional<GraphicsModGroupConfig> graphics_mod_config;

  // Hacks
  bool bEFBAccessEnable = false;
  bool bEFBAccessDeferInvalidation = false;
  bool bPerfQueriesEnable = false;
  bool bBBoxEnable = false;
  bool bCPUCull = false;

  bool bEFBEmulateFormatChanges = false;
  bool bSkipEFBCopyToRam = false;
  bool bSkipXFBCopyToRam = false;
  bool bDisableCopyToVRAM = false;
  bool bDeferEFBCopies = false;
  bool bImmediateXFB = false;
  bool bSkipPresentingDuplicateXFBs = false;
  bool bCopyEFBScaled = false;
  int iSafeTextureCache_ColorSamples = 0;
  float fAspectRatioHackW = 1;  // 1 until the first frame sets it
  float fAspectRatioHackH = 1;
  bool bEnablePixelLighting = false;
  bool bFastDepthCalc = false;
  bool bVertexRounding = false;
  bool bVISkip = false;
  int iEFBAccessTileSize = 0;
  int iSaveTargetId = 0;
  u32 iMissingColorValue = 0;
  bool bFastTextureSampling = false;
#ifdef __APPLE__
  bool bNoMipmapping = false;
#endif

  // Stereoscopy
  StereoMode stereo_mode{};
  bool stereo_per_eye_resolution_full = false;
  float stereo_depth = 0;
  float stereo_convergence = 0;
  bool bStereoSwapEyes = false;
  bool bStereoEFBMonoDepth = false;

  // Adapter index (D3D, Vulkan)
  int iAdapter = 0;

  // Metal settings (kept for config compatibility)
  TriState iManuallyUploadBuffers = TriState::Auto;
  TriState iUsePresentDrawable = TriState::Auto;

  // Vulkan: validation layers, threaded submission, and how many draws pass before a command
  // buffer is submitted early.
  bool bEnableValidationLayer = false;
  bool bBackendMultithreading = true;
  int iCommandBufferExecuteInterval = 0;

  // Shader compilation
  bool bWaitForShadersBeforeStarting = false;
  ShaderCompilationMode iShaderCompilationMode{};

  // Compiler thread counts: 0 = compile on the video thread, -1 = pick from the CPU count.
  int iShaderCompilerThreads = 0;
  int iShaderPrecompilerThreads = 0;

  // Custom driver library (Android)
  std::string customDriverLibraryName;

  VertexLoaderType vertex_loader_type;

  // Derived settings
  bool UseVSForLinePointExpand() const
  {
    // Vertex-shader expansion when supported, required without geometry shaders, otherwise
    // the user's choice.
    if (!g_backend_info.bSupportsVSLinePointExpand)
      return false;
    return !g_backend_info.bSupportsGeometryShaders || bPreferVSForLinePointExpansion;
  }
  bool MultisamplingEnabled() const { return iMultisamples > 1; }
  bool ExclusiveFullscreenEnabled() const
  {
    return g_backend_info.bSupportsExclusiveFullscreen && !bBorderlessFullscreen;
  }
  bool UseGPUTextureDecoding() const
  {
    return g_backend_info.bSupportsGPUTextureDecoding && bEnableGPUTextureDecoding &&
           !bArbitraryMipmapDetection;
  }
  bool UseVertexRounding() const { return bVertexRounding && iEFBScale != 1; }
  // True when manual texture sampling has to cope with textures whose size or layer count
  // differs from what the game set: hi-res texture packs, scaled EFB copies at a raised
  // internal resolution, or stereoscopic copies with two layers.
  bool ManualTextureSamplingWithCustomTextureSizes() const
  {
    if (bFastTextureSampling)
      return false;
    return bHiresTextures || (iEFBScale != 1 && bCopyEFBScaled) || stereo_mode != StereoMode::Off;
  }
  bool UsingUberShaders() const;
  u32 GetShaderCompilerThreads() const;
  u32 GetShaderPrecompilerThreads() const;

  float GetCustomAspectRatio() const
  {
    return static_cast<float>(custom_aspect_width) / static_cast<float>(custom_aspect_height);
  }
};

extern VideoConfig g_Config;
extern VideoConfig g_ActiveConfig;

// Copies g_Config into g_ActiveConfig (once per frame).
void UpdateActiveConfig();
// Applies a changed config to the video objects and fires the config-changed event.
void CheckForConfigChanges();
