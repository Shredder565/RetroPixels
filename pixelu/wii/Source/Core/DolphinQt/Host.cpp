// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Host.h"

#include <string>
#include <vector>

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QLocale>
#include <QThread>

#include <imgui.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/Host.h"
#include "Core/NetPlayProto.h"
#include "Core/State.h"
#include "Core/System.h"

#ifdef HAS_LIBMGBA
#include "DolphinQt/GBAWidget.h"
#endif
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/Settings.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"

#include "UICommon/DiscordPresence.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
// Runs `action` (which touches the graphics backend) while the GPU thread is not running.
//
// This can be reached from a panic alert raised on the CPU or GPU thread, while that thread waits
// for the alert, so the way of stopping the GPU depends on who is asking: pausing the whole core
// from the CPU or GPU thread would wait for that very thread and never return.
template <typename Action>
void WhileGpuIdle(const Action& action)
{
  if (Core::IsGPUThread())
  {
    // The caller is the GPU thread, so the GPU is not doing anything else right now.
    action();
    return;
  }

  auto& system = Core::System::GetInstance();

  if (Core::IsCPUThread())
  {
    // Dual core, called on the CPU thread: hold only the FIFO (the GPU thread). In single core
    // the CPU thread is also the GPU thread and was handled above.
    const bool core_was_running = Core::GetState(system) == Core::State::Running;
    auto& fifo = system.GetFifo();
    fifo.PauseAndLock();
    action();
    fifo.RestoreState(core_was_running);
    return;
  }

  // Any other thread can pause the core as a whole.
  const Core::CPUThreadGuard pause_core(system);
  action();
}

// Emits one of Host's argument-less signals from the GUI thread.
void EmitOnGuiThread(void (Host::*signal)())
{
  QueueOnObject(QApplication::instance(), [signal] { emit(Host::GetInstance()->*signal)(); });
}
}  // namespace

Host::Host()
{
  // Loading a state changes memory under the debugger's feet.
  State::SetOnAfterLoadCallback([] { Host_UpdateDisasmDialog(); });
}

Host::~Host()
{
  State::SetOnAfterLoadCallback(nullptr);
}

Host* Host::GetInstance()
{
  // Deliberately never destroyed: core threads may still call in during shutdown.
  static Host* const instance = new Host();
  return instance;
}

void Host::SetMainWindowHandle(void* handle)
{
  m_main_window = handle;
}

void Host::SetRenderHandle(void* handle)
{
  m_surface_in_main = Config::Get(Config::MAIN_RENDER_TO_MAIN);

  if (handle == m_surface)
    return;
  m_surface = handle;

  // While a game runs the presenter and the input backends have to follow the new window.
  if (!g_presenter)
    return;
  g_presenter->ChangeSurface(handle);
  g_controller_interface.ChangeWindow(handle);
}

bool Host::GetRenderFocus()
{
#ifdef _WIN32
  // Qt reports focus changes a little late; asking Windows directly keeps the key press that
  // switched away from the game from reaching it. When the game is drawn inside the main window
  // (and not fullscreen), the main window is the one that has to be in front.
  const bool main_window_is_surface = m_surface_in_main && !m_is_fullscreen;
  const HWND expected = static_cast<HWND>(main_window_is_surface ? m_main_window.load() :
                                                                   m_surface.load());
  return GetForegroundWindow() == expected;
#else
  return m_has_focus;
#endif
}

void Host::SetRenderFocus(bool focus)
{
  m_has_focus = focus;

  // Exclusive fullscreen is dropped while the game window is in the background and taken again
  // when it comes back (only for a separate render window).
  if (!g_gfx || !m_is_fullscreen || !g_ActiveConfig.ExclusiveFullscreenEnabled())
    return;

  WhileGpuIdle([focus] {
    if (Config::Get(Config::MAIN_RENDER_TO_MAIN))
      return;
    g_gfx->SetFullscreen(focus);
  });
}

bool Host::GetRenderFullFocus()
{
  return m_has_full_focus;
}

void Host::SetRenderFullFocus(bool focus)
{
  m_has_full_focus = focus;
}

bool Host::GetRenderFullscreen()
{
  return m_is_fullscreen;
}

void Host::SetRenderFullscreen(bool fullscreen)
{
  m_is_fullscreen = fullscreen;

  if (!g_gfx || !g_ActiveConfig.ExclusiveFullscreenEnabled())
    return;
  if (g_gfx->IsFullscreen() == fullscreen)
    return;

  WhileGpuIdle([fullscreen] { g_gfx->SetFullscreen(fullscreen); });
}

bool Host::GetGBAFocus()
{
#ifdef HAS_LIBMGBA
  return qobject_cast<GBAWidget*>(QApplication::activeWindow()) != nullptr;
#else
  return false;
#endif
}

bool Host::GetTASInputFocus() const
{
  return m_tas_has_focus;
}

void Host::SetTASInputFocus(const bool focus)
{
  m_tas_has_focus = focus;
}

void Host::ResizeSurface(int, int)
{
  // The presenter asks the window for its size itself.
  if (g_presenter)
    g_presenter->ResizeSurface();
}

// The functions below are the core's side of the interface (declared in Core/Host.h).

std::vector<std::string> Host_GetPreferredLocales()
{
  std::vector<std::string> locales;
  for (const QString& language : QLocale::system().uiLanguages())
    locales.push_back(language.toStdString());
  return locales;
}

void Host_Message(HostMessageID id)
{
  switch (id)
  {
  case HostMessageID::WMUserStop:
    emit Host::GetInstance()->RequestStop();
    break;
  case HostMessageID::WMUserJobDispatch:
    // Waking the GUI thread's event loop is enough: queued jobs run before it sleeps again.
    QAbstractEventDispatcher::instance(qApp->thread())->wakeUp();
    break;
  default:
    break;
  }
}

void Host_UpdateTitle(const std::string& title)
{
  emit Host::GetInstance()->RequestTitle(QString::fromStdString(title));
}

bool Host_RendererHasFocus()
{
  Host* const host = Host::GetInstance();
  return host->GetRenderFocus() || host->GetGBAFocus();
}

bool Host_RendererHasFullFocus()
{
  return Host::GetInstance()->GetRenderFullFocus();
}

bool Host_RendererIsFullscreen()
{
  return Host::GetInstance()->GetRenderFullscreen();
}

bool Host_TASInputHasFocus()
{
  return Host::GetInstance()->GetTASInputFocus();
}

void Host_YieldToUI()
{
  // Only meaningful on the GUI thread; user input stays queued so it cannot re-enter the caller.
  if (QThread::currentThread() != qApp->thread())
    return;
  qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
}

void Host_UpdateDisasmDialog()
{
  // Refreshing the debugger after every single frame step would only slow stepping down.
  if (Settings::Instance().GetIsContinuouslyFrameStepping())
    return;
  EmitOnGuiThread(&Host::UpdateDisasmDialog);
}

void Host_JitCacheInvalidation()
{
  EmitOnGuiThread(&Host::JitCacheInvalidation);
}

void Host_JitProfileDataWiped()
{
  EmitOnGuiThread(&Host::JitProfileDataWiped);
}

void Host_PPCSymbolsChanged()
{
  EmitOnGuiThread(&Host::PPCSymbolsChanged);
}

void Host_PPCBreakpointsChanged()
{
  EmitOnGuiThread(&Host::PPCBreakpointsChanged);
}

void Host_RequestRenderWindowSize(int w, int h)
{
  emit Host::GetInstance()->RequestRenderSize(w, h);
}

bool Host_UIBlocksControllerState()
{
  // An on-screen ImGui widget that is taking keyboard input keeps the game from seeing it, except
  // while paused (where the overlay is not redrawn).
  if (ImGui::GetCurrentContext() == nullptr || !ImGui::GetIO().WantCaptureKeyboard)
    return false;
  return Core::GetState(Core::System::GetInstance()) != Core::State::Paused;
}

void Host_TitleChanged()
{
#ifdef USE_DISCORD_PRESENCE
  // During NetPlay the presence shows the session instead of the game.
  if (NetPlay::IsNetPlayRunning())
    return;
  Discord::UpdateDiscordPresence();
#endif
}

void Host_UpdateDiscordClientID(const std::string& client_id)
{
#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateClientID(client_id);
#endif
}

bool Host_UpdateDiscordPresenceRaw(const std::string& details, const std::string& state,
                                   const std::string& large_image_key,
                                   const std::string& large_image_text,
                                   const std::string& small_image_key,
                                   const std::string& small_image_text,
                                   const int64_t start_timestamp, const int64_t end_timestamp,
                                   const int party_size, const int party_max)
{
#ifdef USE_DISCORD_PRESENCE
  return Discord::UpdateDiscordPresenceRaw(details, state, large_image_key, large_image_text,
                                           small_image_key, small_image_text, start_timestamp,
                                           end_timestamp, party_size, party_max);
#else
  return false;
#endif
}

#ifndef HAS_LIBMGBA
// Without mGBA there is no GBA emulation to host.
std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>)
{
  return nullptr;
}
#endif
