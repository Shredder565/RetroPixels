// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// PixelU-Wii: boots what the command line names, shows it in a window of its own (or none, with
// -p headless), runs it until the game ends, the window is closed or the process is told to
// stop, and exits. RetroPixels starts it this way and docks the window into its own; settings
// come from PixelU-Wii's .ini files, which RetroPixels writes, and from -C overrides. Questions
// are answered "yes"; on Windows, messages show as message boxes, elsewhere on standard error.
//
//     PixelU-Wii [-u USERDIR] [-C System.Section.Key=Value]... [-p PLATFORM] [-e] FILE
//
//     PixelU-Wii [-u USERDIR] --install-wad FILE   (installs a WAD, such as WiiWare DLC, and exits)
//
// -b, -c, -d and -l (options of the earlier program) are accepted and have no effect.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

#include <OptionParser.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Core.h"
#include "Core/Host.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"
#include "DolphinNoGUI/Hotkeys.h"
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

#ifdef _WIN32
BOOL WINAPI OnConsoleEvent(DWORD)
{
  OnStopSignal(0);
  return TRUE;
}

void InstallStopSignals()
{
  // Ctrl+C, Ctrl+Break and closing the console, when there is one.
  SetConsoleCtrlHandler(OnConsoleEvent, TRUE);
}

// A GUI-subsystem program has no console. Started from one (cmd, PowerShell) without redirected
// output, it borrows that console so messages and --help show up there.
void AttachToParentConsole()
{
  const auto usable = [](HANDLE h) { return h != nullptr && h != INVALID_HANDLE_VALUE; };
  if (usable(GetStdHandle(STD_OUTPUT_HANDLE)) || usable(GetStdHandle(STD_ERROR_HANDLE)))
    return;
  if (AttachConsole(ATTACH_PARENT_PROCESS) == FALSE)
    return;
  static_cast<void>(std::freopen("CONOUT$", "w", stdout));
  static_cast<void>(std::freopen("CONOUT$", "w", stderr));
}
#else
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
#endif

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
#ifdef _WIN32
  // Information stays out of the way; warnings and errors need someone to see them.
  if (style != Common::MsgType::Information)
  {
    UINT flags = MB_TOPMOST | (yes_no ? MB_YESNO : MB_OK);
    flags |= style == Common::MsgType::Critical ? MB_ICONERROR :
             style == Common::MsgType::Question ? MB_ICONQUESTION :
                                                  MB_ICONWARNING;
    const int answer = MessageBoxW(nullptr, UTF8ToWString(text).c_str(),
                                   UTF8ToWString(caption).c_str(), flags);
    return !yes_no || answer == IDYES;
  }
#endif
  std::fprintf(stderr, "%s: %s: %s%s\n", StyleName(style), caption, text,
               yes_no ? " [answered yes]" : "");
  std::fflush(stderr);
  return true;
}

// The window platform this system has, when it can show a window now.
std::string DefaultPlatform()
{
#if defined(_WIN32)
  return "win32";
#elif defined(PIXELU_NOGUI_X11)
  // Under Wayland this is XWayland, where RetroPixels can dock the window.
  const char* const display = std::getenv("DISPLAY");
  return display != nullptr && display[0] != '\0' ? "x11" : "headless";
#else
  return "headless";
#endif
}

std::unique_ptr<Platform> CreatePlatform(const std::string& name)
{
  if (name == "headless")
    return Platform::CreateHeadless();
#ifdef _WIN32
  if (name == "win32")
    return Platform::CreateWin32();
#endif
#ifdef PIXELU_NOGUI_X11
  if (name == "x11")
    return Platform::CreateX11();
#endif
  return nullptr;
}

#ifndef _WIN32
// RetroPixels asks for a window it can dock with --rp-embed (not an option of the parser).
void TakeEmbedFlag(int* argc, char* argv[])
{
  int kept = 1;
  for (int i = 1; i < *argc; ++i)
  {
    if (std::strcmp(argv[i], "--rp-embed") == 0)
      setenv("PIXELU_EMBED", "1", 1);
    else
      argv[kept++] = argv[i];
  }
  argv[kept] = nullptr;
  *argc = kept;
}
#endif

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

namespace
{
int Run(int argc, char* argv[])
{
#ifndef _WIN32
  TakeEmbedFlag(&argc, argv);
#endif
  // The GUI options are accepted so that command lines written for the earlier program work.
  const auto parser =
      CommandLineParse::CreateParser(CommandLineParse::ParserOptions::IncludeGUIOptions);
#if defined(_WIN32)
  parser->add_option("-p", "--platform")
      .action("store")
      .choices({"headless", "win32"})
      .help("Where the emulator runs: win32 (a window, the default) or headless (no window)");
#elif defined(PIXELU_NOGUI_X11)
  parser->add_option("-p", "--platform")
      .action("store")
      .choices({"headless", "x11"})
      .help("Where the emulator runs: x11 (a window on $DISPLAY, the default when it is set) or "
            "headless (no window)");
#else
  parser->add_option("-p", "--platform")
      .action("store")
      .choices({"headless"})
      .help("Where the emulator runs: headless (no window, the default)");
#endif
  parser->add_option("--install-wad")
      .dest("install_wad")
      .action("store")
      .metavar("<file>")
      .help("Install a WAD file (a WiiWare title or its add-on content) into the Wii NAND and exit");
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

  if (options.is_set("install_wad"))
  {
    const std::string wad = static_cast<const char*>(options.get("install_wad"));
    Common::RegisterMsgAlertHandler(PrintAlert);
    UICommon::SetUserDirectory(
        options.is_set("user") ? static_cast<const char*>(options.get("user")) : "");
    UICommon::CreateDirectories();
    UICommon::Init();
    const bool installed = WiiUtils::InstallWAD(wad);
    UICommon::Shutdown();
    std::fprintf(stderr, installed ? "Installed %s\n" : "Could not install %s\n", wad.c_str());
    return installed ? 0 : 1;
  }

  const std::string platform_name = options.is_set("platform") ?
                                        static_cast<const char*>(options.get("platform")) :
                                        DefaultPlatform();
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
      const bool inhibit = Config::Get(Config::MAIN_DISABLE_SCREENSAVER);
      if (inhibit)
        UICommon::InhibitScreenSaver(true);
      {
        Hotkeys hotkeys({.toggle_fullscreen = [&platform] { platform->ToggleFullscreen(); },
                         .stop =
                             [&platform] {
                               platform->RequestShutdown();
                               platform->JobsQueued();
                             }});
        platform->MainLoop();
      }
      if (inhibit)
        UICommon::InhibitScreenSaver(false);
    }

    Core::Shutdown(system);
    UICommon::ShutdownControllers();
  }

  s_platform = nullptr;
  UICommon::Shutdown();
  return status;
}
}  // namespace

#ifdef _WIN32
// Windows entry point (a GUI program: no console window): a UTF-8 argv from the wide command line.
int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
  AttachToParentConsole();
  std::vector<std::string> arguments = Common::CommandLineToUtf8Argv(GetCommandLineW());
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (std::string& argument : arguments)
    argv.push_back(argument.data());
  argv.push_back(nullptr);
  return Run(static_cast<int>(arguments.size()), argv.data());
}
#else
int main(int argc, char* argv[])
{
  return Run(argc, argv);
}
#endif
