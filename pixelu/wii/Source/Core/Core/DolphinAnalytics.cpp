// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/DolphinAnalytics.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#if defined(_WIN32)
#include <windows.h>
#include "Common/WindowsRegistry.h"
#endif

#if defined(ANDROID)
#include <functional>
#endif

#if defined(__APPLE__)
#include "Common/CommonFuncs.h"
#endif

#include "Common/Analytics.h"
#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Random.h"
#include "Common/Timer.h"
#include "Common/Version.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/HW/GCPad.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#include "InputCommon/GCAdapter.h"
#include "InputCommon/InputConfig.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
constexpr char kReportEndpoint[] = "https://analytics.dolphin-emu.org/report";

// The name a quirk is reported under. The names are part of the report format.
const char* QuirkReportName(GameQuirk quirk)
{
  switch (quirk)
  {
  case GameQuirk::UsesDVDLowStopLaser: return "uses-DVDLowStopLaser";
  case GameQuirk::UsesDVDLowOffset: return "uses-DVDLowOffset";
  case GameQuirk::UsesDVDLowReadDiskBCA: return "uses-DVDLowReadDiskBca";
  case GameQuirk::UsesDVDLowRequestDiscStatus: return "uses-DVDLowRequestDiscStatus";
  case GameQuirk::UsesDVDLowRequestRetryNumber: return "uses-DVDLowRequestRetryNumber";
  case GameQuirk::UsesDVDLowSerMeasControl: return "uses-DVDLowSerMeasControl";
  case GameQuirk::UsesDifferentPartitionCommand: return "uses-different-partition-command";
  case GameQuirk::UsesDIInterruptMaskCommand: return "uses-di-interrupt-command";
  case GameQuirk::MismatchedGPUTexGensBetweenXFAndBP:
    return "mismatched-gpu-texgens-between-xf-and-bp";
  case GameQuirk::MismatchedGPUColorsBetweenXFAndBP:
    return "mismatched-gpu-colors-between-xf-and-bp";
  case GameQuirk::UsesUncommonWDMode: return "uses-uncommon-wd-mode";
  case GameQuirk::UsesWDUnimplementedIOCtl: return "uses-wd-unimplemented-ioctl";
  case GameQuirk::UsesUnknownBPCommand: return "uses-unknown-bp-command";
  case GameQuirk::UsesUnknownCPCommand: return "uses-unknown-cp-command";
  case GameQuirk::UsesUnknownXFCommand: return "uses-unknown-xf-command";
  case GameQuirk::UsesMaybeInvalidCPCommand: return "uses-maybe-invalid-cp-command";
  case GameQuirk::UsesCPPerfCommand: return "uses-cp-perf-command";
  case GameQuirk::UsesUnimplementedAXCommand: return "uses-unimplemented-ax-command";
  case GameQuirk::UsesAXInitialTimeDelay: return "uses-ax-initial-time-delay";
  case GameQuirk::UsesAXWiimoteLowPass: return "uses-ax-wiimote-lowpass";
  case GameQuirk::UsesAXWiimoteBiquad: return "uses-ax-wiimote-biquad";
  case GameQuirk::SetsXFClipDisableBit0: return "sets-xf-clipdisable-bit-0";
  case GameQuirk::SetsXFClipDisableBit1: return "sets-xf-clipdisable-bit-1";
  case GameQuirk::SetsXFClipDisableBit2: return "sets-xf-clipdisable-bit-2";
  case GameQuirk::MismatchedGPUColorsBetweenCPAndXF:
    return "mismatched-gpu-colors-between-cp-and-xf";
  case GameQuirk::MismatchedGPUNormalsBetweenCPAndXF:
    return "mismatched-gpu-normals-between-cp-and-xf";
  case GameQuirk::MismatchedGPUTexCoordsBetweenCPAndXF:
    return "mismatched-gpu-tex-coords-between-cp-and-xf";
  case GameQuirk::MismatchedGPUMatrixIndicesBetweenCPAndXF:
    return "mismatched-gpu-matrix-indices-between-cp-and-xf";
  case GameQuirk::ReadsBoundingBox: return "reads-bounding-box";
  case GameQuirk::InvalidPositionComponentFormat: return "invalid-position-component-format";
  case GameQuirk::InvalidNormalComponentFormat: return "invalid-normal-component-format";
  case GameQuirk::InvalidTextureCoordinateComponentFormat:
    return "invalid-texture-coordinate-component-format";
  case GameQuirk::InvalidColorComponentFormat: return "invalid-color-component-format";
  case GameQuirk::Count:
    break;
  }
  return "unknown-quirk";
}

constexpr u64 kMicrosecondsPerSecond = 1000000;

#if defined(ANDROID)
std::function<std::string(std::string)> s_android_get_value;
#endif

// The next sampling start: `wait_secs` from now plus up to the jitter.
u64 NextSamplingTime(int wait_secs, int jitter_secs)
{
  const u64 delay = static_cast<u64>(wait_secs) * kMicrosecondsPerSecond +
                    Common::Random::GenerateValue<u64>() %
                        (static_cast<u64>(jitter_secs) * kMicrosecondsPerSecond);
  return Common::Timer::NowUs() + delay;
}

const char* ShaderCompilationModeName()
{
  switch (Config::Get(Config::GFX_SHADER_COMPILATION_MODE))
  {
  case ShaderCompilationMode::AsynchronousUberShaders:
    return "async-ubershaders";
  case ShaderCompilationMode::AsynchronousSkipRendering:
    return "async-skip-rendering";
  case ShaderCompilationMode::SynchronousUberShaders:
    return "sync-ubershaders";
  default:
    return "sync";
  }
}

// Builder fields that are a setting's value as is.
class SettingFields
{
public:
  explicit SettingFields(Common::AnalyticsReportBuilder* builder) : m_builder(builder) {}

  template <typename T>
  SettingFields& Add(std::string_view key, const Config::Info<T>& setting)
  {
    m_builder->AddData(key, Config::Get(setting));
    return *this;
  }

private:
  Common::AnalyticsReportBuilder* m_builder;
};

void AddOSFields(Common::AnalyticsReportBuilder* builder)
{
#if defined(_WIN32)
  builder->AddData("os-type", "windows");
  const auto version = WindowsRegistry::GetOSVersion();
  builder->AddData("win-ver-major", static_cast<u32>(version.dwMajorVersion));
  builder->AddData("win-ver-minor", static_cast<u32>(version.dwMinorVersion));
  builder->AddData("win-ver-build", static_cast<u32>(version.dwBuildNumber));
#elif defined(ANDROID)
  builder->AddData("os-type", "android");
  builder->AddData("android-manufacturer", s_android_get_value("DEVICE_MANUFACTURER"));
  builder->AddData("android-model", s_android_get_value("DEVICE_MODEL"));
  builder->AddData("android-version", s_android_get_value("DEVICE_OS"));
#elif defined(__APPLE__)
  builder->AddData("os-type", "osx");
  const Common::MacOSVersion version = Common::GetMacOSVersion();
  builder->AddData("osx-ver-major", version.major);
  builder->AddData("osx-ver-minor", version.minor);
  builder->AddData("osx-ver-bugfix", version.patch);
#elif defined(__linux__)
  builder->AddData("os-type", "linux");
#elif defined(__FreeBSD__)
  builder->AddData("os-type", "freebsd");
#elif defined(__OpenBSD__)
  builder->AddData("os-type", "openbsd");
#elif defined(__NetBSD__)
  builder->AddData("os-type", "netbsd");
#elif defined(__HAIKU__)
  builder->AddData("os-type", "haiku");
#else
  builder->AddData("os-type", "unknown");
#endif
}

void AddEmulationSettings(Common::AnalyticsReportBuilder* builder)
{
  SettingFields(builder)
      .Add("cfg-dsp-hle", Config::MAIN_DSP_HLE)
      .Add("cfg-dsp-jit", Config::MAIN_DSP_JIT)
      .Add("cfg-dsp-thread", Config::MAIN_DSP_THREAD)
      .Add("cfg-cpu-thread", Config::MAIN_CPU_THREAD)
      .Add("cfg-fastmem", Config::MAIN_FASTMEM)
      .Add("cfg-syncgpu", Config::MAIN_SYNC_GPU)
      .Add("cfg-audio-backend", Config::MAIN_AUDIO_BACKEND)
      .Add("cfg-oc-enable", Config::MAIN_OVERCLOCK_ENABLE)
      .Add("cfg-oc-factor", Config::MAIN_OVERCLOCK)
      .Add("cfg-vi-oc-enable", Config::MAIN_VI_OVERCLOCK_ENABLE)
      .Add("cfg-vi-oc-factor", Config::MAIN_VI_OVERCLOCK)
      .Add("cfg-render-to-main", Config::MAIN_RENDER_TO_MAIN);
  if (g_video_backend != nullptr)
    builder->AddData("cfg-video-backend", g_video_backend->GetConfigName());
}

void AddGraphicsSettings(Common::AnalyticsReportBuilder* builder)
{
  SettingFields settings(builder);
  settings.Add("cfg-gfx-multisamples", Config::GFX_MSAA).Add("cfg-gfx-ssaa", Config::GFX_SSAA);
  builder->AddData("cfg-gfx-anisotropy",
                   std::to_underlying(Config::Get(Config::GFX_ENHANCE_MAX_ANISOTROPY)));
  settings.Add("cfg-gfx-vsync", Config::GFX_VSYNC);
  builder->AddData("cfg-gfx-aspect-ratio", static_cast<int>(Config::Get(Config::GFX_ASPECT_RATIO)));
  settings.Add("cfg-gfx-efb-access", Config::GFX_HACK_EFB_ACCESS_ENABLE)
      .Add("cfg-gfx-efb-copy-format-changes", Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES);
  // The next two (and "immediate-xfb", whose meaning is inverted by mistake) are sent negated;
  // the report history depends on that.
  builder->AddData("cfg-gfx-efb-copy-ram", !Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM));
  builder->AddData("cfg-gfx-xfb-copy-ram", !Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM));
  settings.Add("cfg-gfx-defer-efb-copies", Config::GFX_HACK_DEFER_EFB_COPIES);
  builder->AddData("cfg-gfx-immediate-xfb", !Config::Get(Config::GFX_HACK_IMMEDIATE_XFB));
  settings.Add("cfg-gfx-efb-copy-scaled", Config::GFX_HACK_COPY_EFB_SCALED)
      .Add("cfg-gfx-internal-resolution", Config::GFX_EFB_SCALE)
      .Add("cfg-gfx-tc-samples", Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES);
  builder->AddData("cfg-gfx-stereo-mode", static_cast<int>(Config::Get(Config::GFX_STEREO_MODE)));
  settings.Add("cfg-gfx-stereo-per-eye-resolution-full",
               Config::GFX_STEREO_PER_EYE_RESOLUTION_FULL);
  // Sent as a number, as it always was.
  builder->AddData("cfg-gfx-hdr", static_cast<int>(Config::Get(Config::GFX_ENHANCE_HDR_OUTPUT)));
  settings.Add("cfg-gfx-per-pixel-lighting", Config::GFX_ENABLE_PIXEL_LIGHTING);
  builder->AddData("cfg-gfx-shader-compilation-mode", ShaderCompilationModeName());
  settings.Add("cfg-gfx-wait-for-shaders", Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING)
      .Add("cfg-gfx-fast-depth", Config::GFX_FAST_DEPTH_CALC);
  // Vertex rounding only does anything above native resolution.
  const bool vertex_rounding =
      Config::Get(Config::GFX_HACK_VERTEX_ROUNDING) && Config::Get(Config::GFX_EFB_SCALE) != 1;
  builder->AddData("cfg-gfx-vertex-rounding", vertex_rounding);
}

void AddGPUFeatures(Common::AnalyticsReportBuilder* builder)
{
  const BackendInfo& gpu = g_backend_info;

  const int adapter = Config::Get(Config::GFX_ADAPTER);
  if (adapter < static_cast<int>(gpu.Adapters.size()))
    builder->AddData("gpu-adapter", gpu.Adapters[static_cast<std::size_t>(adapter)]);
  else if (!gpu.AdapterName.empty())
    builder->AddData("gpu-adapter", gpu.AdapterName);

  const std::pair<const char*, bool> features[] = {
      {"gpu-has-exclusive-fullscreen", gpu.bSupportsExclusiveFullscreen},
      {"gpu-has-dual-source-blend", gpu.bSupportsDualSourceBlend},
      {"gpu-has-primitive-restart", gpu.bSupportsPrimitiveRestart},
      {"gpu-has-geometry-shaders", gpu.bSupportsGeometryShaders},
      {"gpu-has-3d-vision", gpu.bSupports3DVision},
      {"gpu-has-early-z", gpu.bSupportsEarlyZ},
      {"gpu-has-binding-layout", gpu.bSupportsBindingLayout},
      {"gpu-has-bbox", gpu.bSupportsBBox},
      {"gpu-has-fragment-stores-and-atomics", gpu.bSupportsFragmentStoresAndAtomics},
      {"gpu-has-gs-instancing", gpu.bSupportsGSInstancing},
      {"gpu-has-post-processing", gpu.bSupportsPostProcessing},
      {"gpu-has-palette-conversion", gpu.bSupportsPaletteConversion},
      {"gpu-has-clip-control", gpu.bSupportsClipControl},
      {"gpu-has-ssaa", gpu.bSupportsSSAA},
      {"gpu-has-logic-ops", gpu.bSupportsLogicOp},
      {"gpu-has-framebuffer-fetch", gpu.bSupportsFramebufferFetch},
  };
  for (const auto& [key, supported] : features)
    builder->AddData(key, supported);
}
}  // namespace

#if defined(ANDROID)
void DolphinAnalytics::AndroidSetGetValFunc(std::function<std::string(std::string)> func)
{
  s_android_get_value = std::move(func);
}
#endif

DolphinAnalytics::DolphinAnalytics()
{
  m_enabled = Config::Get(Config::MAIN_ANALYTICS_ENABLED);

  ReloadConfig();
  MakeBaseBuilder();

  // Only an opt-in change needs the backend swapped.
  m_config_callback = Config::AddConfigChangedCallback([this] {
    const bool enabled = Config::Get(Config::MAIN_ANALYTICS_ENABLED);
    if (enabled == m_enabled)
      return;
    m_enabled = enabled;
    ReloadConfig();
  });
}

DolphinAnalytics::~DolphinAnalytics()
{
  Config::RemoveConfigChangedCallback(m_config_callback);
}

DolphinAnalytics& DolphinAnalytics::Instance()
{
  static DolphinAnalytics analytics;
  return analytics;
}

void DolphinAnalytics::ReloadConfig()
{
  std::lock_guard lock{m_send_mutex};

  // Without consent there is no backend, and reports go nowhere.
  std::unique_ptr<Common::AnalyticsReportingBackend> backend;
  if (m_enabled)
    backend = std::make_unique<Common::HttpAnalyticsBackend>(kReportEndpoint);
  m_reporter.SetBackend(std::move(backend));

  m_unique_id = Config::Get(Config::MAIN_ANALYTICS_ID);
  if (m_unique_id.empty())
    GenerateNewIdentity();
}

void DolphinAnalytics::GenerateNewIdentity()
{
  // 128 random bits as 32 hex digits.
  const u64 high = Common::Random::GenerateValue<u64>();
  const u64 low = Common::Random::GenerateValue<u64>();
  m_unique_id = fmt::format("{:016x}{:016x}", high, low);

  Config::SetBase(Config::MAIN_ANALYTICS_ID, m_unique_id);
  Config::Save();
}

std::string DolphinAnalytics::MakeUniqueId(std::string_view data) const
{
  // The first 64 bits of SHA-1(ID + data), in hex.
  std::string input = m_unique_id;
  input += data;
  const auto digest = Common::SHA1::CalculateDigest(input);

  std::string id;
  for (std::size_t i = 0; i < 8; ++i)
    id += fmt::format("{:02x}", digest[i]);
  return id;
}

void DolphinAnalytics::ReportDolphinStart(std::string_view ui_type)
{
  Common::AnalyticsReportBuilder report(m_base_builder);
  report.AddData("type", "dolphin-start");
  report.AddData("ui-type", ui_type);
  report.AddData("id", MakeUniqueId("dolphin-start"));
  Send(report);
}

void DolphinAnalytics::ReportGameStart()
{
  MakePerGameBuilder();

  Common::AnalyticsReportBuilder report(m_per_game_builder);
  report.AddData("type", "game-start");
  Send(report);

  m_quirk_reported.fill(false);
  InitializePerformanceSampling();
}

void DolphinAnalytics::ReportGameQuirk(GameQuirk quirk)
{
  const auto index = static_cast<std::size_t>(quirk);
  if (m_quirk_reported[index])
    return;
  m_quirk_reported[index] = true;

  Common::AnalyticsReportBuilder report(m_per_game_builder);
  report.AddData("type", "quirk");
  report.AddData("quirk", QuirkReportName(quirk));
  Send(report);
}

void DolphinAnalytics::ReportPerformanceInfo(PerformanceSample sample)
{
  if (ShouldStartPerformanceSampling())
    m_sampling = true;
  if (m_sampling)
    m_samples.push_back(sample);
  if (m_samples.size() < NUM_PERFORMANCE_SAMPLES_PER_REPORT)
    return;

  std::vector<u32> speeds, prims, draw_calls;
  speeds.reserve(m_samples.size());
  prims.reserve(m_samples.size());
  draw_calls.reserve(m_samples.size());
  for (const PerformanceSample& collected : m_samples)
  {
    // Speed in thousandths of full speed.
    speeds.push_back(static_cast<u32>(collected.speed_ratio * 1000));
    prims.push_back(static_cast<u32>(collected.num_prims));
    draw_calls.push_back(static_cast<u32>(collected.num_draw_calls));
  }

  // Samples are only taken while a game runs, so the per-game data exists.
  Common::AnalyticsReportBuilder report(m_per_game_builder);
  report.AddData("type", "performance");
  report.AddData("speed", speeds);
  report.AddData("prims", prims);
  report.AddData("draw-calls", draw_calls);
  Send(report);

  m_samples.clear();
  m_sampling = false;
}

void DolphinAnalytics::InitializePerformanceSampling()
{
  m_samples.clear();
  m_sampling = false;
  m_next_sampling_us = NextSamplingTime(PERFORMANCE_SAMPLING_INITIAL_WAIT_TIME_SECS,
                                        PERFORMANCE_SAMPLING_WAIT_TIME_JITTER_SECS);
}

bool DolphinAnalytics::ShouldStartPerformanceSampling()
{
  if (Common::Timer::NowUs() < m_next_sampling_us)
    return false;

  m_next_sampling_us = NextSamplingTime(PERFORMANCE_SAMPLING_INTERVAL_SECS,
                                        PERFORMANCE_SAMPLING_WAIT_TIME_JITTER_SECS);
  return true;
}

void DolphinAnalytics::MakeBaseBuilder()
{
  Common::AnalyticsReportBuilder builder;

  builder.AddData("version-desc", Common::GetScmDescStr());
  builder.AddData("version-hash", Common::GetScmRevGitStr());
  builder.AddData("version-branch", Common::GetScmBranchStr());
  builder.AddData("version-dist", Common::GetScmDistributorStr());
  SettingFields(&builder).Add("update-track", Config::MAIN_AUTOUPDATE_UPDATE_TRACK);
  builder.AddData("cpu-summary", cpu_info.Summarize());
  AddOSFields(&builder);

  m_base_builder = builder;
}

void DolphinAnalytics::MakePerGameBuilder()
{
  Common::AnalyticsReportBuilder builder(m_base_builder);

  const SConfig& config = SConfig::GetInstance();
  builder.AddData("gameid", config.GetGameID());
  // The ID is derived from the game ID so it differs between games.
  builder.AddData("id", MakeUniqueId(config.GetGameID()));
  builder.AddData("simulated-memory-size", config.GetSimulatedMemorySize());

  AddEmulationSettings(&builder);
  AddGraphicsSettings(&builder);
  AddGPUFeatures(&builder);

  builder.AddData("netplay", NetPlay::IsNetPlayRunning());
  builder.AddData("movie", Core::System::GetInstance().GetMovie().IsMovieActive());

  // Enough to tell keyboard/mouse players from gamepad and GameCube adapter users.
  const bool adapter_detected = GCAdapter::IsDetected(nullptr);
  builder.AddData("gcadapter-detected", adapter_detected);
  builder.AddData("has-controller", Pad::GetConfig()->IsControllerControlledByGamepadDevice(0) ||
                                        GCAdapter::IsDetected(nullptr));

  m_per_game_builder = builder;
}
