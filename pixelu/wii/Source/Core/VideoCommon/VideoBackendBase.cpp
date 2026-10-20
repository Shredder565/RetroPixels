// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/VideoBackendBase.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/DolphinAnalytics.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/System.h"

#ifdef _WIN32
#include "VideoBackends/D3D/VideoBackend.h"
#include "VideoBackends/D3D12/VideoBackend.h"
#endif
#include "VideoBackends/Null/VideoBackend.h"
#ifdef HAS_OPENGL
#include "VideoBackends/OGL/VideoBackend.h"
#include "VideoBackends/Software/VideoBackend.h"
#endif
#ifdef HAS_VULKAN
#include "VideoBackends/Vulkan/VideoBackend.h"
#endif

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/BPStructs.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/EFBInterface.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/Resources/CustomResourceManager.h"
#include "VideoCommon/TMEM.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoState.h"
#include "VideoCommon/Widescreen.h"
#include "VideoCommon/XFStateManager.h"

VideoBackendBase* g_video_backend = nullptr;

#ifdef _WIN32
#include <windows.h>

// Exported markers that ask hybrid-graphics laptops to run the program on the discrete GPU:
// Nvidia Optimus (driver 302 and later) and AMD PowerXpress (13.35 and later) look for them.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace
{
VideoBackendBase* FirstBackend()
{
  const auto& all = VideoBackendBase::GetAvailableBackends();
  return all.empty() ? nullptr : all.front().get();
}

// Logs or alerts the first time only; each call site keeps its own flag.
template <typename F>
void OnFirstCall(bool& done, F&& report)
{
  if (!done)
    report();
  done = true;
}
}  // namespace

std::string VideoBackendBase::BadShaderFilename(const char* shader_stage, int counter)
{
  return fmt::format("{}bad_{}_{}_{}.txt", File::GetUserPath(D_DUMP_IDX), shader_stage,
                     g_video_backend->GetConfigName(), counter);
}

void VideoBackendBase::Video_OutputXFB(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height,
                                       u64 ticks)
{
  // Runs on the CPU thread, from the video interface.
  if (!m_initialized || !g_presenter)
    return;

  auto& system = Core::System::GetInstance();
  auto& core_timing = system.GetCoreTiming();

  // With immediate XFB the presenter shows XFB copies as they happen, not at VI swap time.
  if (!g_ActiveConfig.bImmediateXFB)
  {
    system.GetFifo().SyncGPU(Fifo::SyncGPUReason::Swap);
    const TimePoint present_at = core_timing.GetTargetHostTime(static_cast<s64>(ticks));
    AsyncRequests::GetInstance()->PushEvent([=] {
      g_presenter->ViSwap(xfb_addr, fb_width, fb_stride, fb_height, ticks, present_at);
    });
  }

  // Tell the presenter when the following swap is expected: one refresh period later.
  auto& vi = system.GetVideoInterface();
  const s64 period_den = vi.GetTargetRefreshRateDenominator();
  const s64 period_num = vi.GetTargetRefreshRateNumerator();
  const s64 period_ticks = system.GetSystemTimers().GetTicksPerSecond() * period_den / period_num;
  const u64 next_ticks = ticks + static_cast<u64>(period_ticks);
  const TimePoint next_time = core_timing.GetTargetHostTime(static_cast<s64>(next_ticks));
  AsyncRequests::GetInstance()->PushEvent(
      [=] { g_presenter->SetNextSwapEstimatedTime(next_ticks, next_time); });
}

u32 VideoBackendBase::Video_GetQueryResult(PerfQueryType type)
{
  if (!g_perf_query->ShouldEmulate())
    return 0;

  Core::System::GetInstance().GetFifo().SyncGPU(Fifo::SyncGPUReason::PerfQuery);
  if (!g_perf_query->IsFlushed())
    AsyncRequests::GetInstance()->PushBlockingEvent([] { g_perf_query->FlushResults(); });
  return g_perf_query->GetQueryResult(type);
}

u16 VideoBackendBase::Video_GetBoundingBox(int index)
{
  DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::ReadsBoundingBox);

  if (!g_ActiveConfig.bBBoxEnable)
  {
    static bool logged = false;
    OnFirstCall(logged, [] {
      ERROR_LOG_FMT(VIDEO, "BBox shall be used but it is disabled. Please use a gameini to "
                           "enable it for this game.");
    });
  }
  else if (!g_backend_info.bSupportsBBox)
  {
    static bool alerted = false;
    OnFirstCall(alerted, [] {
      PanicAlertFmtT("This game requires bounding box emulation to run properly but your "
                     "graphics card or its drivers do not support it. As a result you will "
                     "experience bugs or freezes while running this game.");
    });
  }

  Core::System::GetInstance().GetFifo().SyncGPU(Fifo::SyncGPUReason::BBox);
  return AsyncRequests::GetInstance()->PushBlockingEvent(
      [&] { return g_bounding_box->Get(static_cast<u32>(index)); });
}

std::string VideoBackendBase::GetDefaultBackendConfigName()
{
  const VideoBackendBase* backend = FirstBackend();
  return backend != nullptr ? backend->GetConfigName() : "";
}

std::string VideoBackendBase::GetDefaultBackendDisplayName()
{
  const VideoBackendBase* backend = FirstBackend();
  return backend != nullptr ? backend->GetDisplayName() : "";
}

const std::vector<std::unique_ptr<VideoBackendBase>>& VideoBackendBase::GetAvailableBackends()
{
  // Built once. The first entry is the default backend and becomes the active one.
  static const std::vector<std::unique_ptr<VideoBackendBase>> s_backends = [] {
    std::vector<std::unique_ptr<VideoBackendBase>> list;
#ifdef _WIN32
    list.push_back(std::make_unique<DX11::VideoBackend>());
    list.push_back(std::make_unique<DX12::VideoBackend>());
#endif
#ifdef HAS_OPENGL
    list.push_back(std::make_unique<OGL::VideoBackend>());
#endif
#ifdef HAS_VULKAN
    list.push_back(std::make_unique<Vulkan::VideoBackend>());
#endif
#ifdef HAS_OPENGL
    list.push_back(std::make_unique<SW::VideoSoftware>());
#endif
    list.push_back(std::make_unique<Null::VideoBackend>());

    g_video_backend = list.front().get();
    return list;
  }();
  return s_backends;
}

void VideoBackendBase::ActivateBackend(const std::string& name)
{
  // An empty name selects the default; an unknown name leaves the selection as it is.
  if (name.empty())
    g_video_backend = FirstBackend();

  for (const auto& backend : GetAvailableBackends())
  {
    if (backend->GetConfigName() == name)
    {
      g_video_backend = backend.get();
      return;
    }
  }
}

void VideoBackendBase::PopulateBackendInfo(const WindowSystemInfo& wsi)
{
  // Once the core is up the GPU thread owns the backend info; refilling it from the UI thread
  // would race (and is not needed).
  if (!Core::IsUninitialized(Core::System::GetInstance()))
    return;

  g_Config.Refresh();
  // Start from defaults so nothing is left over from another backend.
  g_backend_info = {};
  ActivateBackend(Config::Get(Config::MAIN_GFX_BACKEND));
  g_backend_info.DisplayName = g_video_backend->GetDisplayName();
  g_video_backend->InitBackendInfo(wsi);
  // Settings that depend on the device (adapter, MSAA modes) can only be checked now.
  g_Config.VerifyValidity();
}

void VideoBackendBase::DoState(PointerWrap& p)
{
  auto& system = Core::System::GetInstance();
  if (!system.IsDualCoreMode())
  {
    VideoCommon_DoState(p);
    return;
  }

  AsyncRequests::GetInstance()->PushBlockingEvent([&] { VideoCommon_DoState(p); });
  // Let the GPU thread idle after a load (for example when paused); new FIFO data wakes it.
  system.GetFifo().GpuMaySleep();
}

bool VideoBackendBase::InitializeShared(std::unique_ptr<AbstractGfx> gfx,
                                        std::unique_ptr<VertexManagerBase> vertex_manager,
                                        std::unique_ptr<PerfQueryBase> perf_query,
                                        std::unique_ptr<BoundingBox> bounding_box)
{
  return InitializeShared(std::move(gfx), std::move(vertex_manager), std::move(perf_query),
                          std::move(bounding_box), std::make_unique<HardwareEFBInterface>(),
                          std::make_unique<TextureCacheBase>());
}

bool VideoBackendBase::InitializeShared(std::unique_ptr<AbstractGfx> gfx,
                                        std::unique_ptr<VertexManagerBase> vertex_manager,
                                        std::unique_ptr<PerfQueryBase> perf_query,
                                        std::unique_ptr<BoundingBox> bounding_box,
                                        std::unique_ptr<EFBInterfaceBase> efb_interface,
                                        std::unique_ptr<TextureCacheBase> texture_cache)
{
  std::memset(reinterpret_cast<u8*>(&g_main_cp_state), 0, sizeof(g_main_cp_state));
  std::memset(reinterpret_cast<u8*>(&g_preprocess_cp_state), 0, sizeof(g_preprocess_cp_state));
  s_tex_mem.fill(0);

  // Marks the backend as running (the config dialog checks this).
  m_initialized = true;

  g_gfx = std::move(gfx);
  g_vertex_manager = std::move(vertex_manager);
  g_perf_query = std::move(perf_query);
  g_bounding_box = std::move(bounding_box);
  g_texture_cache = std::move(texture_cache);
  g_efb_interface = std::move(efb_interface);

  g_presenter = std::make_unique<VideoCommon::Presenter>();
  g_frame_dumper = std::make_unique<FrameDumper>();
  g_framebuffer_manager = std::make_unique<FramebufferManager>();
  g_shader_cache = std::make_unique<VideoCommon::ShaderCache>();
  g_graphics_mod_manager = std::make_unique<GraphicsModManager>();
  g_widescreen = std::make_unique<WidescreenManager>();

  // Initialised in this order; the first failure stops the rest.
  const bool ok = g_vertex_manager->Initialize() && g_shader_cache->Initialize() &&
                  g_perf_query->Initialize() && g_presenter->Initialize() &&
                  g_framebuffer_manager->Initialize(g_ActiveConfig.iEFBScale) &&
                  g_texture_cache->Initialize() &&
                  (!g_backend_info.bSupportsBBox || g_bounding_box->Initialize()) &&
                  g_graphics_mod_manager->Initialize();
  if (!ok)
  {
    PanicAlertFmtT("Failed to initialize renderer classes");
    Shutdown();
    return false;
  }

  auto& system = Core::System::GetInstance();
  system.GetCommandProcessor().Init();
  system.GetFifo().Init();
  system.GetPixelEngine().Init();
  BPInit();
  VertexLoaderManager::Init();
  system.GetVertexShaderManager().Init();
  system.GetGeometryShaderManager().Init();
  system.GetPixelShaderManager().Init();
  system.GetXFStateManager().Init();
  TMEM::Init();

  g_Config.VerifyValidity();
  UpdateActiveConfig();

  if (g_Config.bDumpTextures)
  {
    OSD::AddMessage("Texture Dumping is enabled. This will reduce performance.",
                    OSD::Duration::NORMAL);
  }

  g_shader_cache->InitializeShaderCache();
  system.GetCustomResourceManager().Initialize();
  return true;
}

void VideoBackendBase::ShutdownShared()
{
  auto& system = Core::System::GetInstance();
  system.GetCustomResourceManager().Shutdown();

  g_frame_dumper.reset();
  g_presenter.reset();

  if (g_shader_cache)
    g_shader_cache->Shutdown();
  if (g_texture_cache)
    g_texture_cache->Shutdown();

  // Released in dependency order, the device last.
  g_bounding_box.reset();
  g_perf_query.reset();
  g_graphics_mod_manager.reset();
  g_texture_cache.reset();
  g_framebuffer_manager.reset();
  g_shader_cache.reset();
  g_vertex_manager.reset();
  g_efb_interface.reset();
  g_widescreen.reset();
  g_gfx.reset();

  m_initialized = false;

  VertexLoaderManager::Clear();
  system.GetFifo().Shutdown();
}
