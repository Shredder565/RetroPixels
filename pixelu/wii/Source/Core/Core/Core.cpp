// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Core.h"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <utility>
#include <variant>

#include <fmt/chrono.h>
#include <fmt/format.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "AudioCommon/AudioCommon.h"

#include "Common/Assert.h"
#include "Common/CPUDetect.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FatFsUtil.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/OneShotEvent.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/TimeUtil.h"
#include "Common/Version.h"

#include "Core/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/CPUThreadConfigCallback.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/DSPEmulator.h"
#include "Core/DolphinAnalytics.h"
#include "Core/FifoPlayer/FifoPlayer.h"
#include "Core/FreeLookManager.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DSP.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/HW.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/Wiimote.h"
#include "Core/Host.h"
#include "Core/IOS/IOS.h"
#include "Core/MemTools.h"
#include "Core/Movie.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayProto.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/GDBStub.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"

#ifdef USE_MEMORYWATCHER
#include "Core/MemoryWatcher.h"
#endif

#include "DiscIO/RiivolutionPatcher.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"

#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoEvents.h"

namespace Core
{
namespace
{
// --- Session state -----------------------------------------------------------

// Uninitialized -> Starting (Init) -> Running (CPU thread up) -> Stopping
// (Stop) -> Uninitialized (emulation thread exits). Paused is never stored:
// the core is paused when this says Running and the CPU is stepping.
std::atomic<State> s_state = State::Uninitialized;

// Held by any thread that temporarily takes the CPU thread's place, and by
// anyone changing s_state from outside the emulation thread.
std::recursive_mutex s_core_lock;

std::thread s_emulation_thread;
Common::HookableEvent<Core::State> s_state_listeners;

bool s_wants_determinism = false;
bool s_throttler_temp_disabled = false;

// Frame advance: s_frame_step_pending is set when a step starts, and
// s_new_frame_shown is set by the presenter once a genuinely new frame is on
// screen, which ends the step at the next field.
bool s_frame_step_pending = false;
std::atomic<bool> s_new_frame_shown;

thread_local bool tls_is_cpu_thread = false;
thread_local bool tls_is_gpu_thread = false;

#ifdef USE_MEMORYWATCHER
std::unique_ptr<MemoryWatcher> s_memory_watcher;
#endif

// --- Host job queue ----------------------------------------------------------

struct PendingHostJob
{
  std::function<void(Core::System&)> run;
  bool survives_stop;
};
std::mutex s_host_queue_lock;
std::queue<PendingHostJob> s_host_queue;

bool IsStoppingOrStopped(State state)
{
  return state == State::Stopping || state == State::Uninitialized;
}

// Moves Starting to Running, unless Stop() got in first and set Stopping.
void MarkCpuThreadReady()
{
  State expected = State::Starting;
  s_state.compare_exchange_strong(expected, State::Running);
}

void StopAllRumble()
{
  GCAdapter::ResetRumble();
  if (!Pad::IsInitialized())
    return;
  for (int pad = 0; pad < 4; ++pad)
    Pad::ResetRumble(pad);
}

// --- Pausing from other threads --------------------------------------------

// Locks s_core_lock (always) and, if emulation is running, freezes the CPU,
// DSP and GPU so the caller can act as the CPU thread. Returns whether the CPU
// was running before. Must be paired with ResumeAndUnlock.
bool PauseAndLock(Core::System& system)
{
  s_core_lock.lock();
  if (!IsRunning(system))
    return true;

  // The CPU goes first: its thread may be waiting on the audio or GPU thread.
  const bool cpu_was_running = system.GetCPU().PauseAndLock();
  system.GetDSP().GetDSPEmulator()->PauseAndLock();
  system.GetFifo().PauseAndLock();
  StopAllRumble();
  return cpu_was_running;
}

void ResumeAndUnlock(Core::System& system, bool resume_cpu)
{
  Common::ScopeGuard release_lock{[] { s_core_lock.unlock(); }};
  if (!IsRunning(system))
    return;

  system.GetDSP().GetDSPEmulator()->UnpauseAndUnlock();
  StopAllRumble();

  // The CPU is released last. Its unlock also restarts audio and video as
  // needed, and doing it last avoids racing a CPU::Break from those threads.
  system.GetCPU().RestoreStateAndUnlock(resume_cpu);
}

// --- CPU thread ----------------------------------------------------------------

void EnterInitialExecutionState(Core::System& system, bool force_pause)
{
  // The CPU thread starts out stepping and waits for this.
  const bool pause = force_pause || SConfig::GetInstance().bBootToPause;
  SetState(system, pause ? State::Paused : State::Running, true, true);
  Host_UpdateDisasmDialog();
}

// Starts the GDB stub if one is configured (never in hardcore mode); the CPU
// then waits paused for the debugger. Returns whether it did.
bool StartDebuggerStub()
{
  const auto hardcore = [] { return AchievementManager::GetInstance().IsHardcoreModeActive(); };

#ifndef _WIN32
  // A local socket takes precedence over a TCP port.
  std::string socket_path = Config::Get(Config::MAIN_GDB_SOCKET);
  if (!socket_path.empty() && !hardcore())
  {
    GDBStub::InitLocal(socket_path.data());
    return true;
  }
#endif

  const int port = Config::Get(Config::MAIN_GDB_PORT);
  if (port <= 0 || hardcore())
    return false;
  GDBStub::Init(static_cast<u32>(port));
  return true;
}

void StopDebuggerStub()
{
  if (!GDBStub::IsActive())
    return;
  INFO_LOG_FMT(CONSOLE, "{}", StopMessage(true, "Stopping GDB ..."));
  GDBStub::Deinit();
  INFO_LOG_FMT(CONSOLE, "{}", StopMessage(true, "GDB stopped."));
  INFO_LOG_FMT(GDB_STUB, "Killed by CPU shutdown");
}

// Runs the emulated PowerPC until emulation stops. In single core mode this
// thread also runs the GPU.
void RunCpuThread(Core::System& system, const std::optional<std::string>& savestate_path,
                  bool delete_savestate)
{
  Common::SetCurrentThreadName(system.IsDualCoreMode() ? "CPU thread" : "CPU-GPU thread");

  // Has to wait until the video backend is up.
  DolphinAnalytics::Instance().ReportGameStart();
  system.GetPerfMetrics().Reset();

  // Fastmem and the JIT's stack guard depend on catching host faults.
  const bool catch_faults = EMM::IsExceptionHandlerSupported();
  if (catch_faults)
    EMM::InstallExceptionHandler();

#ifdef USE_MEMORYWATCHER
  s_memory_watcher = std::make_unique<MemoryWatcher>();
#endif

  if (savestate_path)
  {
    ::State::LoadAs(system, *savestate_path);
    if (delete_savestate)
      File::Delete(*savestate_path);
  }

  {
    std::unique_lock lock(s_core_lock);
    MarkCpuThreadReady();
  }

  EnterInitialExecutionState(system, StartDebuggerStub());

  system.GetCPU().Run();

#ifdef USE_MEMORYWATCHER
  s_memory_watcher.reset();
#endif

  if (catch_faults)
    EMM::UninstallExceptionHandler();

  StopDebuggerStub();
}

// Plays back a FIFO log instead of running game code.
void RunFifoPlayerThread(Core::System& system, const std::optional<std::string>&, bool)
{
  Common::SetCurrentThreadName(system.IsDualCoreMode() ? "FIFO player thread" :
                                                         "FIFO-GPU thread");

  FifoPlayer& player = system.GetFifoPlayer();
  auto playback_core = player.GetCPUCore();
  if (!playback_core)
  {
    PanicAlertFmt("FIFO file doesn't contain any frame, cannot playback.");
    player.Close();
    return;
  }

  system.GetPowerPC().InjectExternalCPUCore(playback_core.get());
  {
    std::lock_guard lock(s_core_lock);
    MarkCpuThreadReady();
  }
  EnterInitialExecutionState(system, false);

  system.GetCPU().Run();

  system.GetPowerPC().InjectExternalCPUCore(nullptr);
  player.Close();
}

// --- Video backend lifetime --------------------------------------------------

using VideoShutdownGuard = Common::ScopeGuard<Common::MoveOnlyFunction<void()>>;

// Runs on the thread that will own the backend (the GPU thread in dual core).
bool StartVideoBackend(Core::System& system, const WindowSystemInfo& wsi)
{
  DeclareAsGPUThread();
  AsyncRequests::GetInstance()->SetPassthrough(!system.IsDualCoreMode());
  return g_video_backend->Initialize(wsi);
}

void StopVideoBackend()
{
  // Messages still on screen would otherwise survive into the next session.
  OSD::ClearMessages();
  g_video_backend->Shutdown();
}

// Brings the video backend up. The returned guard takes it down again (in
// dual core mode by ending the GPU loop and joining the GPU thread, which shuts
// the backend down itself). Returns null when the backend failed to start.
[[nodiscard]] std::unique_ptr<VideoShutdownGuard> StartVideo(Core::System& system,
                                                             const WindowSystemInfo& wsi)
{
  if (!system.IsDualCoreMode())
  {
    if (!StartVideoBackend(system, wsi))
      return nullptr;
    return std::make_unique<VideoShutdownGuard>(&StopVideoBackend);
  }

  std::promise<bool> started_promise;
  std::future<bool> started = started_promise.get_future();
  std::thread gpu_thread{[&system, wsi, promise = std::move(started_promise)]() mutable {
    Common::SetCurrentThreadName("Video thread");

    const bool ok = StartVideoBackend(system, wsi);
    promise.set_value(ok);
    if (!ok)
      return;

    system.GetFifo().RunGpuLoop();
    INFO_LOG_FMT(CONSOLE, "{}", StopMessage(false, "Video Loop Ended"));
    StopVideoBackend();
  }};

  if (!started.get())
  {
    gpu_thread.join();
    return nullptr;
  }

  return std::make_unique<VideoShutdownGuard>(
      [&system, joined_thread = std::move(gpu_thread)]() mutable {
        INFO_LOG_FMT(CONSOLE, "{}", StopMessage(true, "Wait for Video Loop to exit ..."));
        system.GetFifo().ExitGpuLoop();
        joined_thread.join();
        INFO_LOG_FMT(CONSOLE, "{}", StopMessage(true, "GPU thread stopped."));
      });
}

// --- Emulation thread ----------------------------------------------------------

// Teardown steps of EmuThread, in the order they run (the guards there are
// declared in the opposite order).

void FinishSession()
{
  {
    std::lock_guard lock(s_core_lock);
    s_state.store(State::Uninitialized);
  }
  NotifyStateChanged(State::Uninitialized);
  INFO_LOG_FMT(CONSOLE, "Stop\t\t---- Shutdown complete ----");
}

void WriteSDImageBackToFolder(bool image_was_built_from_folder)
{
  if (!image_was_built_from_folder || !Config::Get(Config::MAIN_ALLOW_SD_WRITES))
    return;

  if (Common::SyncSDImageToSDFolder([] { return false; }))
    return;

  PanicAlertFmtT("Failed to sync SD card with folder. All changes made this session will be "
                 "discarded on next boot if you do not manually re-issue a resync in Config > "
                 "Wii > SD Card Settings > {0}!",
                 Common::GetStringT(Common::SD_UNPACK_TEXT));
}

void ShutdownHardware(Core::System& system)
{
  INFO_LOG_FMT(CONSOLE, "{}", StopMessage(false, "Shutting down HW"));
  HW::Shutdown(system);
  INFO_LOG_FMT(CONSOLE, "{}", StopMessage(false, "HW shutdown"));

  // Per-game and temporary settings may only be dropped once nothing in the
  // hardware can read them any more.
  BootManager::RestoreConfig();

  PatchEngine::Shutdown();
  HLE::Clear();

  CPUThreadGuard guard(system);
  system.GetPowerPC().GetDebugInterface().Clear(guard);
}

// Builds the SD card image from the configured folder when folder sync is on.
// Returns whether it did (and so whether it must be written back at the end).
bool BuildSDImageFromFolder(const Core::System& system)
{
  const bool wanted = system.IsWii() && Config::Get(Config::MAIN_WII_SD_CARD) &&
                      Config::Get(Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC);
  return wanted && Common::SyncSDFolderToSDImage([] { return false; }, WantsDeterminism());
}

void SelectDSPThreading()
{
  // A separate DSP LLE thread only pays off with cores to spare.
  const int cores_needed = cpu_info.HTT ? 4 : 2;
  Config::SetBaseOrCurrent(Config::MAIN_DSP_THREAD, cpu_info.num_cores > cores_needed);
}

void SelectCpuCore(Core::System& system)
{
  const bool use_jit = Config::Get(Config::MAIN_CPU_CORE) != PowerPC::CPUCore::Interpreter;
  system.GetPowerPC().SetMode(use_jit ? PowerPC::CoreMode::JIT : PowerPC::CoreMode::Interpreter);
}

void CountPresentedFrame(const PresentInfo& present_info);

using CpuThreadEntry = void (*)(Core::System&, const std::optional<std::string>&, bool);

// The emulation thread: sets up every subsystem, boots, then becomes the CPU
// thread. Everything it set up is torn down by the scope guards, in reverse.
void EmuThread(Core::System& system, std::unique_ptr<BootParameters> boot, WindowSystemInfo wsi)
{
  NotifyStateChanged(State::Starting);
  Common::ScopeGuard finish_session{&FinishSession};

  Common::SetCurrentThreadName("Emuthread - Starting");
  DeclareAsCPUThread();
  s_frame_step_pending = false;

  // Settings may have changed while no emulation was running.
  CPUThreadConfigCallback::CheckForConfigChanges();

  // Input coordinates are relative to the render window from now on.
  ASSERT(g_controller_interface.IsInit());
  g_controller_interface.ChangeWindow(wsi.render_window);

  Pad::LoadConfig();
  Pad::LoadGBAConfig();
  Keyboard::LoadConfig();
  // Wii Remote mappings are loaded when ES reports the title (OnESTitleChanged).

  BootSessionData session = std::move(boot->boot_session_data);
  const std::optional<std::string>& savestate_path = session.GetSavestatePath();
  const bool delete_savestate = session.GetDeleteSavestate() == DeleteSavestateAfterBoot::Yes;

  const bool sd_synced_from_folder = BuildSDImageFromFolder(system);
  Common::ScopeGuard sd_write_back{
      [sd_synced_from_folder] { WriteSDImageBackToFolder(sd_synced_from_folder); }};

  FreeLook::LoadInputConfig();

  Movie::MovieManager& movie = system.GetMovie();
  movie.Init(*boot);
  Common::ScopeGuard movie_shutdown{[&movie] { movie.Shutdown(); }};

  AudioCommon::InitSoundStream(system);
  Common::ScopeGuard audio_shutdown{[&system] { AudioCommon::ShutdownSoundStream(system); }};

  const auto* netplay_sram =
      NetPlay::IsNetPlayRunning() ? &session.GetNetplaySettings()->sram : nullptr;
  HW::Init(system, netplay_sram);
  Common::ScopeGuard hardware_shutdown{[&system] { ShutdownHardware(system); }};

  const auto video_shutdown = StartVideo(system, wsi);
  if (!video_shutdown)
  {
    PanicAlertFmt("Failed to initialize video backend!");
    return;
  }

  SelectDSPThreading();
  const bool dsp_ok = system.GetDSP().GetDSPEmulator()->Initialize(
      system.IsWii(), Config::Get(Config::MAIN_DSP_THREAD));
  if (!dsp_ok)
  {
    PanicAlertFmt("Failed to initialize DSP emulation!");
    return;
  }

  AudioCommon::PostInitSoundStream(system);

  // Known starting point: CPU, FIFO and audio paused, interpreter selected
  // (booting always runs on the interpreter).
  system.GetCPU().Break();
  system.GetPowerPC().SetMode(PowerPC::CoreMode::Interpreter);

  const bool plays_fifo_log = std::holds_alternative<BootParameters::DFF>(boot->parameters);
  const CpuThreadEntry cpu_entry = plays_fifo_log ? &RunFifoPlayerThread : &RunCpuThread;

  std::optional<DiscIO::Riivolution::SavegameRedirect> save_redirect;
  if (system.IsWii())
    save_redirect = DiscIO::Riivolution::ExtractSavegameRedirect(boot->riivolution_patches);

  {
    ASSERT(IsCPUThread());
    CPUThreadGuard guard(system);
    if (!CBoot::BootUp(system, guard, std::move(boot)))
      return;
  }

  // The Wii file system is prepared after booting, when the running title is
  // known (copying saves needs its directories).
  Common::ScopeGuard wii_fs_cleanup{[&session] {
    CleanUpWiiFileSystemContents(session);
    session.InvokeWiiSyncCleanup();
  }};
  if (system.IsWii())
    InitializeWiiFileSystemContents(save_redirect, session);
  else
    wii_fs_cleanup.Dismiss();

  // From here on CoreTiming may wait on the GPU (SyncGPU).
  system.GetFifo().Prepare();

  const Common::EventHook presented_hook =
      GetVideoEvents().after_present_event.Register(&CountPresentedFrame);

  SelectCpuCore(system);
  UpdateTitle(system);

  cpu_entry(system, savestate_path, delete_savestate);
}

// Presenter callback (GPU thread): frame statistics and the frame-advance signal.
void CountPresentedFrame(const PresentInfo& present_info)
{
  PerformanceMetrics& metrics = Core::System::GetInstance().GetPerfMetrics();
  metrics.CountFrame();
  metrics.SetLatestFramePresentationOffset(present_info.actual_present_time -
                                           present_info.intended_present_time);
  metrics.SetLatestFrameBufferSize(present_info.frame_buffer_width,
                                   present_info.frame_buffer_height);

  // A VI duplicate shows the previous frame again; it does not end a frame step.
  if (present_info.reason != PresentInfo::PresentReason::VideoInterfaceDuplicate)
    s_new_frame_shown.store(true);
}

// --- Screenshots ---------------------------------------------------------------

// User/ScreenShots/<game id>/, or User/ScreenShots/ if that cannot be created.
std::string ScreenshotDirectory()
{
  const std::string& root = File::GetUserPath(D_SCREENSHOTS_IDX);
  std::string per_game = root + SConfig::GetInstance().GetGameID() + DIR_SEP_CHR;
  if (File::CreateFullPath(per_game))
    return per_game;
  return root;
}

// <dir>/<game id>_<local date and time>.png, with _1, _2, ... added when taken.
std::optional<std::string> NextScreenshotPath()
{
  const std::string prefix = ScreenshotDirectory() + SConfig::GetInstance().GetGameID();

  const auto now = Common::LocalTime(std::time(nullptr));
  if (!now)
    return std::nullopt;
  const std::string stem = fmt::format("{}_{:%Y-%m-%d_%H-%M-%S}", prefix, *now);

  std::string path = fmt::format("{}.png", stem);
  for (u32 suffix = 1; File::Exists(path); ++suffix)
    path = fmt::format("{}_{}.png", stem, suffix);
  return path;
}
}  // namespace

// --- Public interface --------------------------------------------------------

bool GetIsThrottlerTempDisabled()
{
  return s_throttler_temp_disabled;
}

void SetIsThrottlerTempDisabled(bool disable)
{
  s_throttler_temp_disabled = disable;
}

void FrameUpdateOnCPUThread()
{
  if (NetPlay::IsNetPlayRunning())
    NetPlay::NetPlayClient::SendTimeBase();
}

void OnFrameEnd([[maybe_unused]] Core::System& system)
{
#ifdef USE_MEMORYWATCHER
  if (!s_memory_watcher)
    return;
  ASSERT(IsCPUThread());
  const CPUThreadGuard guard(system);
  s_memory_watcher->Step(guard);
#endif
}

std::string StopMessage(bool main_thread, std::string_view message)
{
  const char* const thread_name = main_thread ? "Main Thread" : "Video Thread";
  return fmt::format("Stop [{} {}]\t{}", thread_name, Common::CurrentThreadId(), message);
}

void DisplayMessage(std::string message, int time_in_ms)
{
  if (!IsRunningOrStarting(Core::System::GetInstance()))
    return;

  // The on-screen font only covers printable ASCII.
  if (!std::ranges::all_of(message, Common::IsPrintableCharacter))
    return;

  OSD::AddMessage(std::move(message), static_cast<u32>(time_in_ms));
}

bool IsRunning(Core::System&)
{
  return s_state.load() == State::Running;
}

bool IsRunningOrStarting(Core::System&)
{
  const State state = s_state.load();
  return state == State::Starting || state == State::Running;
}

bool IsUninitialized(Core::System&)
{
  return s_state.load() == State::Uninitialized;
}

bool IsCPUThread()
{
  return tls_is_cpu_thread;
}

bool IsGPUThread()
{
  return tls_is_gpu_thread;
}

bool WantsDeterminism()
{
  return s_wants_determinism;
}

// Thread-role markers, set by each thread for itself.
void DeclareAsCPUThread() { tls_is_cpu_thread = true; }
void UndeclareAsCPUThread() { tls_is_cpu_thread = false; }
void DeclareAsGPUThread() { tls_is_gpu_thread = true; }
void UndeclareAsGPUThread() { tls_is_gpu_thread = false; }

// Host thread. BootManager::BootCore prepares the session and ends up here.
bool Init(Core::System& system, std::unique_ptr<BootParameters> boot, const WindowSystemInfo& wsi)
{
  std::lock_guard lock(s_core_lock);

  if (s_emulation_thread.joinable())
  {
    if (!IsUninitialized(system))
    {
      PanicAlertFmtT("Emu Thread already running");
      return false;
    }
    // The previous session has ended; collect its thread.
    s_emulation_thread.join();
  }

  // Anything left over from the previous session.
  HostDispatchJobs(system);

  INFO_LOG_FMT(BOOT, "Starting core = {} mode", system.IsWii() ? "Wii" : "GameCube");
  INFO_LOG_FMT(BOOT, "CPU Thread separate = {}", system.IsDualCoreMode() ? "Yes" : "No");

  // A game INI may have picked another video backend.
  VideoBackendBase::PopulateBackendInfo(wsi);

  // Some backends need window setup on this (the main) thread.
  WindowSystemInfo window = wsi;
  g_video_backend->PrepareWindow(window);

  s_state.store(State::Starting);
  s_emulation_thread = std::thread(EmuThread, std::ref(system), std::move(boot), window);
  return true;
}

// Host thread.
void Stop(Core::System& system)
{
  {
    std::lock_guard lock(s_core_lock);
    if (IsStoppingOrStopped(s_state.load()))
      return;
    s_state.store(State::Stopping);
  }

  NotifyStateChanged(State::Stopping);
  AchievementManager::GetInstance().CloseGame();

  // Jobs that do not survive a stop are dropped here.
  HostDispatchJobs(system);

  system.GetFifo().EmulatorState(false);

  INFO_LOG_FMT(CONSOLE, "Stop [Main Thread]\t\t---- Shutting down ----");
  INFO_LOG_FMT(CONSOLE, "{}", StopMessage(true, "Stop CPU"));
  system.GetCPU().Stop();
}

void Shutdown(Core::System& system)
{
  // The emulation thread is joined only here, after the UI has gone, because
  // DXGI needs the UI thread to keep handling window messages while the
  // swap chain is torn down.
  if (s_emulation_thread.joinable())
    s_emulation_thread.join();

  // Nothing may be left in the queue if the process is about to exit.
  HostDispatchJobs(system);
}

void SetState(Core::System& system, State state, bool report_state_change,
              [[maybe_unused]] bool override_achievement_restrictions)
{
  {
    std::lock_guard lock(s_core_lock);

    // Nothing to pause or resume before the CPU thread is up.
    if (s_state.load() != State::Running)
      return;

    if (state == State::Paused)
    {
#ifdef USE_RETRO_ACHIEVEMENTS
      if (!override_achievement_restrictions && !AchievementManager::GetInstance().CanPause())
        return;
#endif
      // GetState reports Paused from here on, even before the CPU has stopped.
      system.GetCPU().SetStepping(true);
      Wiimote::Pause();
      StopAllRumble();
#ifdef USE_RETRO_ACHIEVEMENTS
      AchievementManager::GetInstance().DoIdle();
#endif
    }
    else if (state == State::Running)
    {
      system.GetCPU().SetStepping(false);
      Wiimote::Resume();
    }
    else
    {
      PanicAlertFmt("Invalid state");
    }
  }

  // Momentary changes (frame advance) skip the notification so the UI does
  // not flicker between paused and running.
  if (report_state_change)
    NotifyStateChanged(GetState(system));
}

State GetState(Core::System& system)
{
  const State state = s_state.load();
  if (state == State::Running && system.GetCPU().IsStepping())
    return State::Paused;
  return state;
}

void SaveScreenShot()
{
  const Core::CPUThreadGuard guard(Core::System::GetInstance());
  if (const std::optional<std::string> path = NextScreenshotPath())
    g_frame_dumper->SaveScreenshot(*path);
}

void SaveScreenShot(std::string_view name)
{
  const Core::CPUThreadGuard guard(Core::System::GetInstance());
  g_frame_dumper->SaveScreenshot(fmt::format("{}{}.png", ScreenshotDirectory(), name));
}

void RunOnCPUThread(Core::System& system, Common::MoveOnlyFunction<void()> function)
{
  if (IsCPUThread())
  {
    function();
    return;
  }

  const bool cpu_was_running = PauseAndLock(system);
  if (IsRunning(system))
    system.GetCPU().AddCPUThreadJob(std::move(function));
  else
    function();  // No CPU thread exists to race with.
  ResumeAndUnlock(system, cpu_was_running);
}

// CPU thread, from VideoInterface at every field.
void Callback_NewField(Core::System& system)
{
  if (s_frame_step_pending)
  {
    // The GPU queue may still hold the swap that presents the new frame, so
    // wait for it before looking at s_new_frame_shown.
    AsyncRequests::GetInstance()->WaitForEmptyQueue();

    if (s_new_frame_shown.load())
    {
      s_frame_step_pending = false;
      system.GetCPU().Break();
      NotifyStateChanged(Core::GetState(system));
    }
  }

  AchievementManager::GetInstance().DoFrame();
}

void UpdateTitle(Core::System& system)
{
  const char* const threading = system.IsDualCoreMode() ? "DC" : "SC";
  const char* const dsp_kind = Config::Get(Config::MAIN_DSP_HLE) ? "HLE" : "LLE";
  const std::string settings =
      fmt::format("{} {} | {} | {}", system.GetPowerPC().GetCPUName(), threading,
                  g_video_backend->GetDisplayName(), dsp_kind);

  // PixelU branding: the window title names PixelU Wii and the Dolphin release it is based on.
  std::string title = fmt::format("PixelU Wii (Dolphin {}) | {}", Common::GetScmDescStr(), settings);
  if (Config::Get(Config::MAIN_SHOW_ACTIVE_TITLE))
  {
    const std::string game = SConfig::GetInstance().GetTitleDescription();
    if (!game.empty())
      title += " | " + game;
  }

  Host_UpdateTitle(title);
}

Common::EventHook AddOnStateChangedCallback(StateChangedCallbackFunc callback)
{
  return s_state_listeners.Register(std::move(callback));
}

void NotifyStateChanged(const Core::State state)
{
  s_state_listeners.Trigger(state);
}

void UpdateWantDeterminism(Core::System& system, bool initial)
{
  const Core::CPUThreadGuard guard(system);

  // Not a setting of its own: movies and NetPlay imply it, and the settings
  // that depend on it (GPU determinism and so on) have their own overrides.
  const bool wanted = system.GetMovie().IsMovieActive() || NetPlay::IsNetPlayRunning();
  if (!initial && wanted == s_wants_determinism)
    return;

  NOTICE_LOG_FMT(COMMON, "Want determinism <- {}", wanted ? "true" : "false");
  s_wants_determinism = wanted;

  if (auto* const ios = system.GetIOS())
    ios->UpdateWantDeterminism(wanted);
  system.GetFifo().UpdateWantDeterminism(wanted);

  // Compiled code depends on it (FMA use, for one).
  system.GetJitInterface().ClearCache(guard);
}

void QueueHostJob(std::function<void(Core::System&)> job, bool run_during_stop)
{
  if (!job)
    return;

  bool queue_was_empty;
  {
    std::lock_guard lock(s_host_queue_lock);
    queue_was_empty = s_host_queue.empty();
    s_host_queue.push(PendingHostJob{std::move(job), run_during_stop});
  }

  // Only the first job needs to wake the host; it drains the whole queue.
  if (queue_was_empty)
    Host_Message(HostMessageID::WMUserJobDispatch);
}

void HostDispatchJobs(Core::System& system)
{
  // Host thread only. Re-entrant: a job may call Stop(), which dispatches too.
  std::unique_lock lock(s_host_queue_lock);
  while (!s_host_queue.empty())
  {
    PendingHostJob next = std::move(s_host_queue.front());
    s_host_queue.pop();

    if (!next.survives_stop && IsStoppingOrStopped(s_state.load()))
      continue;

    lock.unlock();
    next.run(system);
    lock.lock();
  }
}

// Host thread.
void DoFrameStep(Core::System& system)
{
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
  {
    OSD::AddMessage("Frame stepping is disabled in RetroAchievements hardcore mode");
    return;
  }

  std::lock_guard lock(s_core_lock);
  if (GetState(system) == State::Paused)
  {
    // Paused: run until the next new frame (see Callback_NewField).
    s_new_frame_shown = false;
    s_frame_step_pending = true;
    SetState(system, State::Running, false);
  }
  else if (!s_frame_step_pending)
  {
    // Running: the first press only pauses.
    SetState(system, State::Paused);
  }
}

void UpdateInputGate(bool require_focus, bool require_full_focus)
{
  // Without require_focus, input passes even in the background or under a
  // menu, and the host is not asked at all.
  bool focus_ok = true;
  bool full_focus_ok = true;
  if (require_focus)
  {
    focus_ok = (Host_RendererHasFocus() || Host_TASInputHasFocus()) &&
               !Host_UIBlocksControllerState();
    if (require_full_focus)
      full_focus_ok = focus_ok && Host_RendererHasFullFocus();
  }
  ControlReference::SetInputGate(focus_ok && full_focus_ok);
}

CPUThreadGuard::CPUThreadGuard(Core::System& system)
    : m_system(system), m_was_cpu_thread(IsCPUThread())
{
  if (!m_was_cpu_thread)
    m_was_unpaused = PauseAndLock(system);
}

CPUThreadGuard::~CPUThreadGuard()
{
  if (!m_was_cpu_thread)
    ResumeAndUnlock(m_system, m_was_unpaused);
}
}  // namespace Core
