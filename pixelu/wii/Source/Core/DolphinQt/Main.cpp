// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef _WIN32
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>
#endif

#ifdef __linux__
#include <cstdlib>
#include <cstring>
#endif

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <OptionParser.h>
#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QObject>
#include <QPushButton>
#include <QWidget>

#include "Common/Config/Config.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/System.h"

#include "DolphinQt/Host.h"
#include "DolphinQt/MainWindow.h"
#include "DolphinQt/QtUtils/AnalyticsPrompt.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/RunOnObject.h"
#ifdef _WIN32
#include "DolphinQt/QtUtils/SetWindowDecorations.h"
#endif
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/Translation.h"
#include "DolphinQt/Updater.h"

#include "UICommon/CommandLineParse.h"
#include "UICommon/UICommon.h"

namespace
{
// While a panic alert raised by the emulated CPU or GPU thread is on screen, the GUI thread acts
// as that thread: those threads are blocked waiting for the answer, and code run from the dialog
// (focus changes reaching Host, CPU thread guards) must not wait for them in turn.
class StandInForCoreThreads
{
public:
  StandInForCoreThreads(bool cpu, bool gpu) : m_cpu(cpu), m_gpu(gpu)
  {
    if (m_cpu)
      Core::DeclareAsCPUThread();
    if (m_gpu)
      Core::DeclareAsGPUThread();
  }
  ~StandInForCoreThreads()
  {
    if (m_gpu)
      Core::UndeclareAsGPUThread();
    if (m_cpu)
      Core::UndeclareAsCPUThread();
  }
  StandInForCoreThreads(const StandInForCoreThreads&) = delete;
  StandInForCoreThreads& operator=(const StandInForCoreThreads&) = delete;

private:
  bool m_cpu;
  bool m_gpu;
};

QMessageBox::Icon IconFor(Common::MsgType style)
{
  QMessageBox::Icon icon = QMessageBox::NoIcon;
  switch (style)
  {
  case Common::MsgType::Critical:
    icon = QMessageBox::Critical;
    break;
  case Common::MsgType::Warning:
    icon = QMessageBox::Warning;
    break;
  case Common::MsgType::Question:
    icon = QMessageBox::Question;
    break;
  case Common::MsgType::Information:
    icon = QMessageBox::Information;
    break;
  }
  return icon;
}

// Shows the core's alerts and questions as message boxes on the GUI thread. Warnings offer to
// silence further panic alerts for this session.
bool ShowCoreAlert(const char* caption, const char* text, bool yes_no, Common::MsgType style)
{
  const bool from_cpu_thread = Core::IsCPUThread();
  const bool from_gpu_thread = Core::IsGPUThread();

  const std::optional<bool> answer = RunOnObject(QApplication::instance(), [&] {
    const StandInForCoreThreads stand_in(from_cpu_thread, from_gpu_thread);

    ModalMessageBox box(QApplication::activeWindow(), Qt::ApplicationModal);
    box.setWindowTitle(QString::fromUtf8(caption));
    box.setText(QString::fromUtf8(text));
    box.setStandardButtons(yes_no ? QMessageBox::Yes | QMessageBox::No : QMessageBox::Ok);
    if (style == Common::MsgType::Warning)
      box.addButton(QMessageBox::Ignore)->setText(QObject::tr("Ignore for this session"));
    box.setIcon(IconFor(style));

    switch (box.exec())
    {
    case QMessageBox::Yes:
      return true;
    case QMessageBox::Ignore:
      Config::SetCurrent(Config::MAIN_USE_PANIC_HANDLERS, false);
      return true;
    default:
      return false;
    }
  });
  return answer.value_or(false);
}

#ifdef _WIN32
// A GUI-subsystem program has no console. When started from a console (cmd, PowerShell) without
// redirected output, borrow that console so log output and --help text show up there.
void AttachToParentConsole()
{
  const auto usable = [](HANDLE handle) {
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
  };
  if (usable(::GetStdHandle(STD_OUTPUT_HANDLE)) || usable(::GetStdHandle(STD_ERROR_HANDLE)))
    return;
  if (AttachConsole(ATTACH_PARENT_PROCESS) == FALSE)
    return;
  static_cast<void>(freopen("CONOUT$", "w", stdout));
  static_cast<void>(freopen("CONOUT$", "w", stderr));
}
#endif

#ifdef __linux__
void PrepareLinuxPlatform()
{
#if (QT_VERSION >= QT_VERSION_CHECK(6, 3, 0))
  // Qt 6.3 and later swallow mouse input meant for our XInput2 code unless Qt's own XInput2
  // support is turned off.
  setenv("QT_XCB_NO_XI2", "1", true);
#endif
  // PixelU: run on X11, which under a Wayland desktop means XWayland: the game window can only
  // be docked into RetroPixels there, and the keyboard and mouse backend is X11's. An explicit
  // Wayland choice is overridden too. Only without any X server (no DISPLAY) is Wayland left to
  // Qt, so the program still starts; games then draw through the video backends' Wayland
  // surfaces, without docking.
  const char* const display = getenv("DISPLAY");
  const char* const wayland = getenv("WAYLAND_DISPLAY");
  if ((display == nullptr || display[0] == '\0') && wayland != nullptr && wayland[0] != '\0')
    return;
  const char* const platform = getenv("QT_QPA_PLATFORM");
  const bool asked_for_wayland =
      platform != nullptr && Common::CaseInsensitiveContains(platform, "wayland");
  setenv("QT_QPA_PLATFORM", "xcb", asked_for_wayland);
}

// PixelU: "--rp-embed" (RetroPixels' flag for its engines) is the same as PIXELU_EMBED=1: the
// game window waits to be docked into RetroPixels (RenderWidget). It is taken out of argv here,
// since neither Qt nor the option parser knows it.
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

// What the command line asks to boot, if anything.
struct BootRequest
{
  std::unique_ptr<BootParameters> boot;
  bool game_specified = false;
};

BootRequest ReadBootRequest(const optparse::Values& options, const std::vector<std::string>& args,
                            const std::optional<std::string>& save_state)
{
  BootRequest request;

  if (options.is_set("exec"))
  {
    const std::list<std::string> listed = options.all("exec");
    const std::vector<std::string> paths(listed.begin(), listed.end());
    request.boot = BootParameters::GenerateFromFile(
        paths, BootSessionData(save_state, DeleteSavestateAfterBoot::No));
    request.game_specified = true;
  }
  else if (options.is_set("nand_title"))
  {
    // A NAND title is given as its 16-digit hexadecimal ID.
    const std::string title = static_cast<const char*>(options.get("nand_title"));
    if (title.length() == 16)
    {
      const u64 title_id = std::stoull(title, nullptr, 16);
      request.boot = std::make_unique<BootParameters>(BootParameters::NANDTitle{title_id});
    }
    else
    {
      ModalMessageBox::critical(nullptr, QObject::tr("Error"), QObject::tr("Invalid title ID."));
    }
    request.game_specified = true;
  }
  else if (!args.empty())
  {
    request.boot = BootParameters::GenerateFromFile(
        args.front(), BootSessionData(save_state, DeleteSavestateAfterBoot::No));
    request.game_specified = true;
  }

  return request;
}

int RunApplication(int argc, char* argv[])
{
#ifdef _WIN32
  AttachToParentConsole();
#endif

#ifdef __APPLE__
  // The first launch from Finder appends a "-psn_..." process serial number argument, which the
  // option parser would reject.
  if (strncmp(argv[argc - 1], "-psn", 4) == 0)
    --argc;
#endif

#ifdef __linux__
  TakeEmbedFlag(&argc, argv);
  PrepareLinuxPlatform();
#endif

  QCoreApplication::setOrganizationName(QStringLiteral("Dolphin Emulator"));
  QCoreApplication::setOrganizationDomain(QStringLiteral("dolphin-emu.org"));
  QCoreApplication::setApplicationName(QStringLiteral("dolphin-emu"));

  // Qt removes the arguments meant for itself from argc/argv here.
  QApplication app(argc, argv);

  const auto parser =
      CommandLineParse::CreateParser(CommandLineParse::ParserOptions::IncludeGUIOptions);
  const optparse::Values& options = CommandLineParse::ParseArguments(parser.get(), argc, argv);
  const std::vector<std::string> args = parser->args();

#ifdef _WIN32
  QtUtils::InstallWindowDecorationFilter(&app);
  FreeConsole();
#endif

  UICommon::SetUserDirectory(static_cast<const char*>(options.get("user")));
  UICommon::CreateDirectories();
  UICommon::Init();
  Resources::Init();
  Settings::Instance().SetBatchModeEnabled(options.is_set("batch"));

  Common::RegisterMsgAlertHandler(ShowCoreAlert);
  Translation::Initialize();

  // Run the core's queued host jobs whenever the GUI thread is about to go idle.
  QObject::connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock,
                   &app, [] { Core::HostDispatchJobs(Core::System::GetInstance()); });

  std::optional<std::string> save_state;
  if (options.is_set("save_state"))
    save_state = static_cast<const char*>(options.get("save_state"));

  BootRequest request = ReadBootRequest(options, args, save_state);
  const bool batch = Settings::Instance().IsBatchModeEnabled();

  int exit_code = 1;
  if (save_state && !request.game_specified)
  {
    ModalMessageBox::critical(
        nullptr, QObject::tr("Error"),
        QObject::tr("A save state cannot be loaded without specifying a game to launch."));
  }
  else if (batch && !request.game_specified)
  {
    ModalMessageBox::critical(
        nullptr, QObject::tr("Error"),
        QObject::tr("Batch mode cannot be used without specifying a game to launch."));
  }
  else if (!request.boot && (batch || save_state))
  {
    // The game given could not be used; that has been reported already.
  }
  else
  {
    DolphinAnalytics::Instance().ReportDolphinStart("qt");

    Settings::Instance().InitDefaultPalette();
    Settings::Instance().ApplyStyle();

    MainWindow win{Core::System::GetInstance(), std::move(request.boot),
                   static_cast<const char*>(options.get("movie"))};

#if defined(USE_ANALYTICS) && USE_ANALYTICS
    if (!Config::Get(Config::MAIN_ANALYTICS_PERMISSION_ASKED))
    {
      // Ask once the application is active: only then is the main window in its final place, so
      // the prompt can be centred on it. The helper object ends the connection after one use.
      auto* const once = new QObject(&win);
      QObject::connect(qApp, &QGuiApplication::applicationStateChanged, once,
                       [once, &win](Qt::ApplicationState state) {
                         if (state != Qt::ApplicationState::ApplicationActive)
                           return;
                         delete once;
                         ShowAnalyticsPrompt(&win);
                       });
    }
#endif

    if (!Settings::Instance().IsBatchModeEnabled())
    {
      auto* const updater = new Updater(&win, Config::Get(Config::MAIN_AUTOUPDATE_UPDATE_TRACK),
                                        Config::Get(Config::MAIN_AUTOUPDATE_HASH_OVERRIDE));
      updater->start();
    }

    exit_code = app.exec();
  }

  Core::Shutdown(Core::System::GetInstance());
  UICommon::Shutdown();
  Host::GetInstance()->deleteLater();

  return exit_code;
}
}  // namespace

#ifdef _WIN32
// Windows entry point: rebuild a UTF-8 argv from the wide command line.
int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
  std::vector<std::string> arguments = Common::CommandLineToUtf8Argv(GetCommandLineW());
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments)
    argv.push_back(argument.data());
  return RunApplication(static_cast<int>(argv.size()), argv.data());
}
#else
int main(int argc, char* argv[])
{
  return RunApplication(argc, argv);
}
#endif
