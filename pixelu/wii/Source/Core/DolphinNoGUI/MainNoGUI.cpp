// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// dolphin-emu-nogui: PixelU-Wii without a user interface. It boots what the command line names
// (the same options as the Qt program, minus the GUI ones), runs it until the game ends or the
// process gets SIGINT/SIGTERM, and exits. Messages that would be dialog boxes go to standard
// error and are answered "yes".
//
//     dolphin-emu-nogui [-u USERDIR] [-v Null] [-p headless|x11] [-e] FILE

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <signal.h>

#include <OptionParser.h>

#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Core.h"
#include "Core/Host.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "UICommon/CommandLineParse.h"
#include "UICommon/UICommon.h"

namespace
{
// The platform MainLoop() runs on; the signal handler and the Host_* calls reach it here.
Platform* s_platform = nullptr;

void OnStopSignal(int)
{
  // atomic_flag is the one type the standard promises is lock-free, so usable here.
  static std::atomic_flag signalled = ATOMIC_FLAG_INIT;
  if (signalled.test_and_set())
  {
    // A second Ctrl+C while the first one is still being handled: give up at once.
    std::_Exit(1);
  }
  if (s_platform)
    s_platform->RequestShutdown();
}

void InstallStopSignals()
{
  struct sigaction action = {};
  action.sa_handler = OnStopSignal;
  sigemptyset(&action.sa_mask);
  // Interrupted system calls restart; the main loop sees the request within its 10 ms tick.
  action.sa_flags = SA_RESTART;
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
  // A closed terminal or pipe is not a reason to crash.
  std::signal(SIGPIPE, SIG_IGN);
}

const char* StyleName(Common::MsgType style)
{
  switch (style)
  {
  case Common::MsgType::Information:
    return "Information";
  case Common::MsgType::Question:
    return "Question";
  case Common::MsgType::Warning:
    return "Warning";
  case Common::MsgType::Critical:
    return "Critical";
  }
  return "Message";
}

bool PrintAlert(const char* caption, const char* text, bool yes_no, Common::MsgType style)
{
  std::fprintf(stderr, "%s: %s: %s%s\n", StyleName(style), caption, text,
               yes_no ? " [answered yes]" : "");
  std::fflush(stderr);
  return true;
}

std::unique_ptr<Platform> CreatePlatform(const std::string& name)
{
  if (name.empty() || name == "headless")
    return Platform::CreateHeadless();
#ifdef PIXELU_NOGUI_X11
  if (name == "x11")
    return Platform::CreateX11();
#endif
  return nullptr;
}

// What the command line asks to boot, or nullptr (with a message printed) if nothing usable.
std::unique_ptr<BootParameters> ReadBoot(const optparse::Values& options,
                                         const std::vector<std::string>& args)
{
  std::optional<std::string> save_state;
  if (options.is_set("save_state"))
    save_state = static_cast<const char*>(options.get("save_state"));

  if (options.is_set("exec"))
  {
    const std::list<std::string> listed = options.all("exec");
    std::vector<std::string> paths(listed.begin(), listed.end());
    return BootParameters::GenerateFromFile(
        std::move(paths), BootSessionData(save_state, DeleteSavestateAfterBoot::No));
  }

  if (options.is_set("nand_title"))
  {
    const std::string title = static_cast<const char*>(options.get("nand_title"));
    char* end = nullptr;
    const u64 title_id = std::strtoull(title.c_str(), &end, 16);
    if (title.size() != 16 || end != title.c_str() + title.size())
    {
      std::fprintf(stderr, "Invalid title ID \"%s\" (16 hexadecimal digits expected).\n",
                   title.c_str());
      return nullptr;
    }
    return std::make_unique<BootParameters>(BootParameters::NANDTitle{title_id});
  }

  if (!args.empty())
  {
    return BootParameters::GenerateFromFile(
        args.front(), BootSessionData(save_state, DeleteSavestateAfterBoot::No));
  }

  std::fputs("Nothing to boot: name a game or program (or use --exec or --nand_title).\n", stderr);
  return nullptr;
}
}  // namespace

// The core's calls into the front end (Core/Host.h). Without a user interface most of them have
// nothing to do.

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}

bool Host_UIBlocksControllerState()
{
  return false;
}

bool Host_RendererHasFocus()
{
  return true;
}

bool Host_RendererHasFullFocus()
{
  return true;
}

bool Host_RendererIsFullscreen()
{
  return false;
}

bool Host_TASInputHasFocus()
{
  return false;
}

void Host_Message(HostMessageID id)
{
  if (!s_platform)
    return;
  switch (id)
  {
  case HostMessageID::WMUserStop:
    s_platform->RequestShutdown();
    s_platform->JobsQueued();
    break;
  case HostMessageID::WMUserJobDispatch:
    s_platform->JobsQueued();
    break;
  }
}

void Host_PPCSymbolsChanged()
{
}

void Host_PPCBreakpointsChanged()
{
}

void Host_RequestRenderWindowSize(int width, int height)
{
  if (s_platform)
    s_platform->RequestWindowSize(width, height);
}

void Host_UpdateDisasmDialog()
{
}

void Host_JitCacheInvalidation()
{
}

void Host_JitProfileDataWiped()
{
}

void Host_UpdateTitle(const std::string& title)
{
  if (s_platform)
    s_platform->SetTitle(title);
}

void Host_YieldToUI()
{
}

void Host_TitleChanged()
{
}

void Host_UpdateDiscordClientID(const std::string&)
{
}

bool Host_UpdateDiscordPresenceRaw(const std::string&, const std::string&, const std::string&,
                                   const std::string&, const std::string&, const std::string&,
                                   const int64_t, const int64_t, const int, const int)
{
  return false;
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>)
{
  return nullptr;
}

int main(int argc, char* argv[])
{
  const auto parser =
      CommandLineParse::CreateParser(CommandLineParse::ParserOptions::OmitGUIOptions);
#ifdef PIXELU_NOGUI_X11
  parser->add_option("-p", "--platform")
      .action("store")
      .choices({"headless", "x11"})
      .help("Where the emulator runs: headless (no window, the default) or x11 (a window on "
            "$DISPLAY)");
#else
  parser->add_option("-p", "--platform")
      .action("store")
      .choices({"headless"})
      .help("Where the emulator runs: headless (no window, the default)");
#endif
  // optparse prints its own message for a bad command line and then throws the exit status.
  const optparse::Values* parsed = nullptr;
  try
  {
    parsed = &CommandLineParse::ParseArguments(parser.get(), argc, argv);
  }
  catch (int status)
  {
    return status;
  }
  const optparse::Values& options = *parsed;
  const std::vector<std::string> args = parser->args();

  const std::string platform_name =
      options.is_set("platform") ? static_cast<const char*>(options.get("platform")) : "";
  std::unique_ptr<Platform> platform = CreatePlatform(platform_name);
  if (!platform)
  {
    std::fprintf(stderr, "Unknown platform \"%s\".\n", platform_name.c_str());
    return 1;
  }

  std::unique_ptr<BootParameters> boot = ReadBoot(options, args);
  if (!boot)
    return 1;

  Common::RegisterMsgAlertHandler(PrintAlert);

  UICommon::SetUserDirectory(
      options.is_set("user") ? static_cast<const char*>(options.get("user")) : "");
  UICommon::CreateDirectories();
  UICommon::Init();

  s_platform = platform.get();
  InstallStopSignals();

  int status = 0;
  if (!platform->Init())
  {
    std::fputs("The platform could not be set up.\n", stderr);
    status = 1;
  }
  else
  {
    const WindowSystemInfo wsi = platform->GetWindowSystemInfo();
    UICommon::InitControllers(wsi);

    Core::System& system = Core::System::GetInstance();
    if (!BootManager::BootCore(system, std::move(boot), wsi))
    {
      std::fputs("Could not boot the game.\n", stderr);
      status = 1;
    }
    else
    {
      platform->MainLoop();
    }

    Core::Shutdown(system);
    UICommon::ShutdownControllers();
  }

  s_platform = nullptr;
  UICommon::Shutdown();
  return status;
}
