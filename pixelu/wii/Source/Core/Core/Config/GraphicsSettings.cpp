// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Config/GraphicsSettings.h"

#include <string>

#include "VideoCommon/VideoConfig.h"

namespace Config
{
namespace
{
// All of these live in the GFX system (GFX.ini).
Location GfxKey(std::string_view section, std::string_view key)
{
  return Location{System::GFX, std::string(section), std::string(key)};
}

// The widescreen heuristic compares a frame's aspect ratio against 4:3 and 16:9 (expressed
// relative to 4:3).
constexpr float WIDESCREEN_RATIO_OVER_STANDARD = (16 / 9.f) / (4 / 3.f);
}  // namespace

// [Hardware]

const Info<bool> GFX_VSYNC{GfxKey("Hardware", "VSync"), false};
const Info<int> GFX_ADAPTER{GfxKey("Hardware", "Adapter"), 0};

// [Settings]: aspect ratio and cropping

const Info<bool> GFX_WIDESCREEN_HACK{GfxKey("Settings", "wideScreenHack"), false};
const Info<AspectMode> GFX_ASPECT_RATIO{GfxKey("Settings", "AspectRatio"), AspectMode::Auto};
const Info<int> GFX_CUSTOM_ASPECT_RATIO_WIDTH{GfxKey("Settings", "CustomAspectRatioWidth"), 1};
const Info<int> GFX_CUSTOM_ASPECT_RATIO_HEIGHT{GfxKey("Settings", "CustomAspectRatioHeight"), 1};
const Info<AspectMode> GFX_SUGGESTED_ASPECT_RATIO{GfxKey("Settings", "SuggestedAspectRatio"),
                                                  AspectMode::Auto};
const Info<u32> GFX_WIDESCREEN_HEURISTIC_TRANSITION_THRESHOLD{
    GfxKey("Settings", "WidescreenHeuristicTransitionThreshold"), 3};
const Info<float> GFX_WIDESCREEN_HEURISTIC_ASPECT_RATIO_SLOP{
    GfxKey("Settings", "WidescreenHeuristicAspectRatioSlop"), 0.11f};
const Info<float> GFX_WIDESCREEN_HEURISTIC_STANDARD_RATIO{
    GfxKey("Settings", "WidescreenHeuristicStandardRatio"), 1.f};
const Info<float> GFX_WIDESCREEN_HEURISTIC_WIDESCREEN_RATIO{
    GfxKey("Settings", "WidescreenHeuristicWidescreenRatio"), WIDESCREEN_RATIO_OVER_STANDARD};
const Info<bool> GFX_CROP_TO_ASPECT_RATIO{GfxKey("Settings", "Crop"), false};
const Info<bool> GFX_CROP_CUSTOM{GfxKey("Settings", "CropCustom"), false};
const Info<int> GFX_CROP_CUSTOM_LEFT{GfxKey("Settings", "CropCustomLeft"), 0};
const Info<int> GFX_CROP_CUSTOM_TOP{GfxKey("Settings", "CropCustomTop"), 0};
const Info<int> GFX_CROP_CUSTOM_RIGHT{GfxKey("Settings", "CropCustomRight"), 0};
const Info<int> GFX_CROP_CUSTOM_BOTTOM{GfxKey("Settings", "CropCustomBottom"), 0};
const Info<int> GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES{
    GfxKey("Settings", "SafeTextureCacheColorSamples"), 128};

// [Settings]: on-screen statistics

const Info<bool> GFX_SHOW_FPS{GfxKey("Settings", "ShowFPS"), false};
const Info<bool> GFX_SHOW_FTIMES{GfxKey("Settings", "ShowFTimes"), false};
const Info<bool> GFX_SHOW_VPS{GfxKey("Settings", "ShowVPS"), false};
const Info<bool> GFX_SHOW_VTIMES{GfxKey("Settings", "ShowVTimes"), false};
const Info<bool> GFX_SHOW_GRAPHS{GfxKey("Settings", "ShowGraphs"), false};
const Info<bool> GFX_SHOW_SPEED{GfxKey("Settings", "ShowSpeed"), false};
const Info<bool> GFX_SHOW_SPEED_COLORS{GfxKey("Settings", "ShowSpeedColors"), true};
const Info<bool> GFX_MOVABLE_PERFORMANCE_METRICS{GfxKey("Settings", "MovablePerformanceMetrics"),
                                                 false};
const Info<int> GFX_PERF_SAMP_WINDOW{GfxKey("Settings", "PerfSampWindowMS"), 1000};
const Info<bool> GFX_SHOW_NETPLAY_PING{GfxKey("Settings", "ShowNetPlayPing"), false};
const Info<bool> GFX_SHOW_NETPLAY_MESSAGES{GfxKey("Settings", "ShowNetPlayMessages"), false};
const Info<bool> GFX_LOG_RENDER_TIME_TO_FILE{GfxKey("Settings", "LogRenderTimeToFile"), false};
const Info<bool> GFX_OVERLAY_STATS{GfxKey("Settings", "OverlayStats"), false};
const Info<bool> GFX_OVERLAY_PROJ_STATS{GfxKey("Settings", "OverlayProjStats"), false};
const Info<bool> GFX_OVERLAY_SCISSOR_STATS{GfxKey("Settings", "OverlayScissorStats"), false};
const Info<bool> GFX_SHOW_INTERNAL_RESOLUTION{GfxKey("Settings", "ShowInternalResolution"), false};

// [Settings]: texture and frame dumping

const Info<bool> GFX_DUMP_TEXTURES{GfxKey("Settings", "DumpTextures"), false};
const Info<bool> GFX_DUMP_MIP_TEXTURES{GfxKey("Settings", "DumpMipTextures"), true};
const Info<bool> GFX_DUMP_BASE_TEXTURES{GfxKey("Settings", "DumpBaseTextures"), true};
const Info<int> GFX_TEXTURE_PNG_COMPRESSION_LEVEL{GfxKey("Settings", "TexturePNGCompressionLevel"),
                                                  6};
const Info<bool> GFX_HIRES_TEXTURES{GfxKey("Settings", "HiresTextures"), false};
const Info<bool> GFX_CACHE_HIRES_TEXTURES{GfxKey("Settings", "CacheHiresTextures"), false};
const Info<bool> GFX_DUMP_EFB_TARGET{GfxKey("Settings", "DumpEFBTarget"), false};
const Info<bool> GFX_DUMP_XFB_TARGET{GfxKey("Settings", "DumpXFBTarget"), false};
const Info<bool> GFX_DUMP_FRAMES_AS_IMAGES{GfxKey("Settings", "DumpFramesAsImages"), false};
const Info<bool> GFX_USE_LOSSLESS{GfxKey("Settings", "UseLossless"), false};
const Info<std::string> GFX_DUMP_FORMAT{GfxKey("Settings", "DumpFormat"), "avi"};
const Info<std::string> GFX_DUMP_CODEC{GfxKey("Settings", "DumpCodec"), ""};
const Info<std::string> GFX_DUMP_PIXEL_FORMAT{GfxKey("Settings", "DumpPixelFormat"), ""};
const Info<std::string> GFX_DUMP_ENCODER{GfxKey("Settings", "DumpEncoder"), ""};
const Info<std::string> GFX_DUMP_PATH{GfxKey("Settings", "DumpPath"), ""};
const Info<int> GFX_BITRATE_KBPS{GfxKey("Settings", "BitrateKbps"), 25000};
const Info<FrameDumpResolutionType> GFX_FRAME_DUMPS_RESOLUTION_TYPE{
    GfxKey("Settings", "FrameDumpsResolutionType"),
    FrameDumpResolutionType::XFBAspectRatioCorrectedResolution};
const Info<int> GFX_PNG_COMPRESSION_LEVEL{GfxKey("Settings", "PNGCompressionLevel"), 6};

// [Settings]: rendering

const Info<bool> GFX_ENABLE_GPU_TEXTURE_DECODING{GfxKey("Settings", "EnableGPUTextureDecoding"),
                                                 false};
const Info<bool> GFX_ENABLE_PIXEL_LIGHTING{GfxKey("Settings", "EnablePixelLighting"), false};
const Info<bool> GFX_FAST_DEPTH_CALC{GfxKey("Settings", "FastDepthCalc"), true};
const Info<u32> GFX_MSAA{GfxKey("Settings", "MSAA"), 1};
const Info<bool> GFX_SSAA{GfxKey("Settings", "SSAA"), false};
const Info<int> GFX_EFB_SCALE{GfxKey("Settings", "InternalResolution"), 1};
const Info<int> GFX_MAX_EFB_SCALE{GfxKey("Settings", "MaxInternalResolution"), 12};
const Info<bool> GFX_TEXFMT_OVERLAY_ENABLE{GfxKey("Settings", "TexFmtOverlayEnable"), false};
const Info<bool> GFX_TEXFMT_OVERLAY_CENTER{GfxKey("Settings", "TexFmtOverlayCenter"), false};
const Info<bool> GFX_ENABLE_WIREFRAME{GfxKey("Settings", "WireFrame"), false};
const Info<bool> GFX_DISABLE_FOG{GfxKey("Settings", "DisableFog"), false};
const Info<bool> GFX_BORDERLESS_FULLSCREEN{GfxKey("Settings", "BorderlessFullscreen"), false};
const Info<bool> GFX_ENABLE_VALIDATION_LAYER{GfxKey("Settings", "EnableValidationLayer"), false};

// [Settings]: backend threading and shaders

const Info<bool> GFX_BACKEND_MULTITHREADING{GfxKey("Settings", "BackendMultithreading"), true};
const Info<int> GFX_COMMAND_BUFFER_EXECUTE_INTERVAL{
    GfxKey("Settings", "CommandBufferExecuteInterval"), 100};
const Info<bool> GFX_SHADER_CACHE{GfxKey("Settings", "ShaderCache"), true};
const Info<bool> GFX_WAIT_FOR_SHADERS_BEFORE_STARTING{
    GfxKey("Settings", "WaitForShadersBeforeStarting"), false};
const Info<ShaderCompilationMode> GFX_SHADER_COMPILATION_MODE{
    GfxKey("Settings", "ShaderCompilationMode"), ShaderCompilationMode::Synchronous};
const Info<int> GFX_SHADER_COMPILER_THREADS{GfxKey("Settings", "ShaderCompilerThreads"), 1};
const Info<int> GFX_SHADER_PRECOMPILER_THREADS{GfxKey("Settings", "ShaderPrecompilerThreads"), -1};
const Info<bool> GFX_SAVE_TEXTURE_CACHE_TO_STATE{GfxKey("Settings", "SaveTextureCacheToState"),
                                                 true};
const Info<bool> GFX_PREFER_VS_FOR_LINE_POINT_EXPANSION{
    GfxKey("Settings", "PreferVSForLinePointExpansion"), false};
const Info<bool> GFX_CPU_CULL{GfxKey("Settings", "CPUCull"), false};

// [Settings]: Metal-only options (kept so the keys survive in shared INIs)

const Info<TriState> GFX_MTL_MANUALLY_UPLOAD_BUFFERS{GfxKey("Settings", "ManuallyUploadBuffers"),
                                                     TriState::Auto};
const Info<TriState> GFX_MTL_USE_PRESENT_DRAWABLE{GfxKey("Settings", "MTLUsePresentDrawable"),
                                                  TriState::Auto};

// [Settings]: software renderer debugging, GLES, mods, drivers and the vertex loader

const Info<bool> GFX_SW_DUMP_OBJECTS{GfxKey("Settings", "SWDumpObjects"), false};
const Info<bool> GFX_SW_DUMP_TEV_STAGES{GfxKey("Settings", "SWDumpTevStages"), false};
const Info<bool> GFX_SW_DUMP_TEV_TEX_FETCHES{GfxKey("Settings", "SWDumpTevTexFetches"), false};
const Info<bool> GFX_PREFER_GLES{GfxKey("Settings", "PreferGLES"), false};
const Info<bool> GFX_MODS_ENABLE{GfxKey("Settings", "EnableMods"), false};
const Info<std::string> GFX_DRIVER_LIB_NAME{GfxKey("Settings", "DriverLibName"), ""};
const Info<VertexLoaderType> GFX_VERTEX_LOADER_TYPE{GfxKey("Settings", "VertexLoaderType"),
                                                    VertexLoaderType::Native};

// [Enhancements]

const Info<TextureFilteringMode> GFX_ENHANCE_FORCE_TEXTURE_FILTERING{
    GfxKey("Enhancements", "ForceTextureFiltering"), TextureFilteringMode::Default};
const Info<AnisotropicFilteringMode> GFX_ENHANCE_MAX_ANISOTROPY{
    GfxKey("Enhancements", "MaxAnisotropy"), AnisotropicFilteringMode::Default};
const Info<OutputResamplingMode> GFX_ENHANCE_OUTPUT_RESAMPLING{
    GfxKey("Enhancements", "OutputResampling"), OutputResamplingMode::Default};
const Info<std::string> GFX_ENHANCE_POST_SHADER{GfxKey("Enhancements", "PostProcessingShader"), ""};
const Info<bool> GFX_ENHANCE_FORCE_TRUE_COLOR{GfxKey("Enhancements", "ForceTrueColor"), true};
const Info<bool> GFX_ENHANCE_DISABLE_COPY_FILTER{GfxKey("Enhancements", "DisableCopyFilter"), true};
const Info<bool> GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION{
    GfxKey("Enhancements", "ArbitraryMipmapDetection"), false};
const Info<float> GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD{
    GfxKey("Enhancements", "ArbitraryMipmapDetectionThreshold"), 14.0f};
const Info<bool> GFX_ENHANCE_HDR_OUTPUT{GfxKey("Enhancements", "HDROutput"), false};

// [ColorCorrection]

const Info<bool> GFX_CC_CORRECT_COLOR_SPACE{GfxKey("ColorCorrection", "CorrectColorSpace"), false};
const Info<ColorCorrectionRegion> GFX_CC_GAME_COLOR_SPACE{
    GfxKey("ColorCorrection", "GameColorSpace"), ColorCorrectionRegion::SMPTE_NTSCM};
const Info<bool> GFX_CC_CORRECT_GAMMA{GfxKey("ColorCorrection", "CorrectGamma"), false};
const Info<float> GFX_CC_GAME_GAMMA{GfxKey("ColorCorrection", "GameGamma"), 2.35f};
const Info<bool> GFX_CC_SDR_DISPLAY_GAMMA_SRGB{GfxKey("ColorCorrection", "SDRDisplayGammaSRGB"),
                                               true};
const Info<float> GFX_CC_SDR_DISPLAY_CUSTOM_GAMMA{
    GfxKey("ColorCorrection", "SDRDisplayCustomGamma"), 2.2f};
const Info<float> GFX_CC_HDR_PAPER_WHITE_NITS{GfxKey("ColorCorrection", "HDRPaperWhiteNits"),
                                              203.f};

// [Stereoscopy]

const Info<StereoMode> GFX_STEREO_MODE{GfxKey("Stereoscopy", "StereoMode"), StereoMode::Off};
const Info<bool> GFX_STEREO_PER_EYE_RESOLUTION_FULL{
    GfxKey("Stereoscopy", "StereoPerEyeResolutionFull"), false};
const Info<float> GFX_STEREO_DEPTH{GfxKey("Stereoscopy", "StereoDepth"), 20};
const Info<float> GFX_STEREO_CONVERGENCE_PERCENTAGE{
    GfxKey("Stereoscopy", "StereoConvergencePercentage"), 100};
const Info<bool> GFX_STEREO_SWAP_EYES{GfxKey("Stereoscopy", "StereoSwapEyes"), false};
const Info<float> GFX_STEREO_CONVERGENCE{GfxKey("Stereoscopy", "StereoConvergence"), 20};
const Info<bool> GFX_STEREO_EFB_MONO_DEPTH{GfxKey("Stereoscopy", "StereoEFBMonoDepth"), false};
const Info<float> GFX_STEREO_DEPTH_PERCENTAGE{GfxKey("Stereoscopy", "StereoDepthPercentage"), 100};

// [Hacks]

const Info<bool> GFX_HACK_EFB_ACCESS_ENABLE{GfxKey("Hacks", "EFBAccessEnable"), false};
const Info<bool> GFX_HACK_EFB_DEFER_INVALIDATION{GfxKey("Hacks", "EFBAccessDeferInvalidation"),
                                                 false};
const Info<int> GFX_HACK_EFB_ACCESS_TILE_SIZE{GfxKey("Hacks", "EFBAccessTileSize"), 64};
const Info<bool> GFX_HACK_BBOX_ENABLE{GfxKey("Hacks", "BBoxEnable"), false};
const Info<bool> GFX_HACK_FORCE_PROGRESSIVE{GfxKey("Hacks", "ForceProgressive"), true};
const Info<bool> GFX_HACK_SKIP_EFB_COPY_TO_RAM{GfxKey("Hacks", "EFBToTextureEnable"), true};
const Info<bool> GFX_HACK_SKIP_XFB_COPY_TO_RAM{GfxKey("Hacks", "XFBToTextureEnable"), true};
const Info<bool> GFX_HACK_DISABLE_COPY_TO_VRAM{GfxKey("Hacks", "DisableCopyToVRAM"), false};
const Info<bool> GFX_HACK_DEFER_EFB_COPIES{GfxKey("Hacks", "DeferEFBCopies"), true};
const Info<bool> GFX_HACK_IMMEDIATE_XFB{GfxKey("Hacks", "ImmediateXFBEnable"), false};
const Info<bool> GFX_HACK_CAP_IMMEDIATE_XFB{GfxKey("Hacks", "CapImmediateXFB"), false};
const Info<bool> GFX_HACK_SKIP_DUPLICATE_XFBS{GfxKey("Hacks", "SkipDuplicateXFBs"), true};
const Info<bool> GFX_HACK_EARLY_XFB_OUTPUT{GfxKey("Hacks", "EarlyXFBOutput"), true};
const Info<bool> GFX_HACK_COPY_EFB_SCALED{GfxKey("Hacks", "EFBScaledCopy"), true};
const Info<bool> GFX_HACK_EFB_EMULATE_FORMAT_CHANGES{GfxKey("Hacks", "EFBEmulateFormatChanges"),
                                                     false};
const Info<bool> GFX_HACK_VERTEX_ROUNDING{GfxKey("Hacks", "VertexRounding"), false};
const Info<bool> GFX_HACK_VI_SKIP{GfxKey("Hacks", "VISkip"), false};
const Info<u32> GFX_HACK_MISSING_COLOR_VALUE{GfxKey("Hacks", "MissingColorValue"), 0xFFFFFFFF};
const Info<bool> GFX_HACK_FAST_TEXTURE_SAMPLING{GfxKey("Hacks", "FastTextureSampling"), true};
#ifdef __APPLE__
const Info<bool> GFX_HACK_NO_MIPMAPPING{GfxKey("Hacks", "NoMipmapping"), false};
#endif

// [GameSpecific]

const Info<bool> GFX_PERF_QUERIES_ENABLE{GfxKey("GameSpecific", "PerfQueriesEnable"), false};

}  // namespace Config
