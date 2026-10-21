// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "UICommon/AutoUpdate.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <string>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Common/HttpRequest.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"

#ifdef _WIN32
#include <windows.h>

#include "Common/CommonFuncs.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"

// Only Windows ships the updater program.
#define HAS_UPDATER_PROGRAM 1
#else
#include <unistd.h>
#endif

namespace
{
std::atomic_bool s_checking = false;
bool s_updater_launched = false;

std::string ServerRoot()
{
  // Overridable for testing the update flow against another server.
  if (const char* custom = std::getenv("DOLPHIN_UPDATE_SERVER_URL"))
    return custom;
  return "https://dolphin-emu.org";
}

const char* PlatformName()
{
#if defined(_WIN32) && defined(_M_ARM_64)
  return "win-arm64";
#elif defined(_WIN32)
  return "win";
#else
  return "unknown";
#endif
}

u32 CurrentProcessId()
{
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return static_cast<u32>(getpid());
#endif
}

// Reads a string member, inserting null for a missing key; a non-string member throws, as the
// response is only trusted to have the documented shape.
const std::string& Text(picojson::object& object, const char* key)
{
  return object[key].get<std::string>();
}

picojson::object& Child(picojson::object& object, const char* key)
{
  return object[key].get<picojson::object>();
}

// Builds the changelog shown in the update prompt. Versions with their own HTML notes become a
// headed section; the rest become one line each (linked to the pull request if known). English
// only, like the change descriptions themselves.
std::string ChangelogHtml(const picojson::array& versions)
{
  std::string html;
  for (const picojson::value& entry : versions)
  {
    if (!entry.is<picojson::object>())
      continue;
    picojson::object version = entry.get<picojson::object>();

    if (!version["changelog_html"].is<picojson::null>())
    {
      if (!html.empty())
        html += "<hr>";
      html += fmt::format("<b>Dolphin {}</b>", Text(version, "shortrev"));
      html += fmt::format("<p>{}</p>", Text(version, "changelog_html"));
      continue;
    }

    if (!html.empty())
      html += "<div style=\"margin-top: 0.4em;\"></div>";

    if (version["pr_url"].is<std::string>())
      html +=
          fmt::format("<a href=\"{}\">{}</a>", Text(version, "pr_url"), Text(version, "shortrev"));
    else
      html += Text(version, "shortrev");

    const std::string description = Common::GetEscapedHtml(Text(version, "short_descr"));
    html += fmt::format(" by <a href = \"{}\">{}</a> &mdash; {}", Text(version, "author_url"),
                        Text(version, "author"), description);
  }
  return html;
}

#ifdef HAS_UPDATER_PROGRAM
std::string UpdaterExecutable()
{
  return File::GetExeDirectory() + DIR_SEP + "Updater.exe";
}

// The updater's arguments: each flag as "--name=value" in double quotes (inner quotes escaped),
// in name order, each followed by a space.
std::string UpdaterCommandLine(const std::map<std::string, std::string>& flags)
{
  std::string line = UpdaterExecutable() + ' ';
  for (const auto& [name, value] : flags)
  {
    const std::string argument = ReplaceAll(fmt::format("--{}={}", name, value), "\"", "\\\"");
    line += fmt::format("\"{}\" ", argument);
  }
  return line;
}

void RemoveLeftoversOfLastUpdate()
{
  // Old updaters logged next to the executable instead of in the user's log folder.
  File::Delete(File::GetExeDirectory() + DIR_SEP + "Updater.log",
               File::IfAbsentBehavior::NoConsoleWarning);
}

bool LaunchUpdater(const std::string& command_line)
{
  STARTUPINFO startup{};
  startup.cb = sizeof(startup);
  // Keep the busy cursor from appearing while the updater waits in the background.
  startup.dwFlags = STARTF_FORCEOFFFEEDBACK;
  PROCESS_INFORMATION process;

  std::wstring writable_line = UTF8ToWString(command_line);
  if (!CreateProcessW(UTF8ToWString(UpdaterExecutable()).c_str(), writable_line.data(), nullptr,
                      nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
  {
    const std::string reason = Common::GetLastErrorString();
    CriticalAlertFmtT("Could not start updater process: {0}", reason);
    return false;
  }

  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}
#endif
}  // namespace

bool AutoUpdateChecker::SystemSupportsAutoUpdates()
{
#if defined(AUTOUPDATE) && defined(HAS_UPDATER_PROGRAM)
  return true;
#else
  return false;
#endif
}

void AutoUpdateChecker::CheckForUpdate(std::string_view update_track,
                                       std::string_view hash_override, const CheckType check_type)
{
  bool idle = false;
  if (!s_checking.compare_exchange_strong(idle, true))
    return;

  struct ClearOnExit
  {
    ~ClearOnExit() { s_checking.store(false); }
  } clear_on_exit;

  const bool manual = check_type == CheckType::Manual;

  if (s_updater_launched)
  {
    if (manual)
      SuccessAlertFmtT("A Dolphin update is already scheduled for the next time it closes.");
    return;
  }

  if (!SystemSupportsAutoUpdates() || update_track.empty())
    return;

#ifdef HAS_UPDATER_PROGRAM
  RemoveLeftoversOfLastUpdate();
#endif

  const std::string_view own_hash =
      hash_override.empty() ? std::string_view(Common::GetScmRevGitStr()) : hash_override;
  const std::string query = fmt::format("{}/update/check/v1/{}/{}/{}", ServerRoot(), update_track,
                                        own_hash, PlatformName());

  Common::HttpRequest http{std::chrono::seconds{10}};
  const Common::HttpRequest::Response reply = http.Get(query);
  if (!reply)
  {
    if (manual)
      CriticalAlertFmtT("Unable to contact update server.");
    return;
  }

  const std::string body(reply->begin(), reply->end());
  INFO_LOG_FMT(COMMON, "Auto-update JSON response: {}", body);

  picojson::value parsed;
  if (const std::string error = picojson::parse(parsed, body); !error.empty())
  {
    CriticalAlertFmtT("Invalid JSON received from auto-update service : {0}", error);
    return;
  }
  picojson::object root = parsed.get<picojson::object>();

  if (Text(root, "status") != "outdated")
  {
    if (manual)
      SuccessAlertFmtT("You are running the latest version available on this update track.");
    INFO_LOG_FMT(COMMON, "Auto-update status: we are up to date.");
    return;
  }

  picojson::object& current = Child(root, "old");
  picojson::object& next = Child(root, "new");

  NewVersionInformation info;
  info.this_manifest_url = Text(current, "manifest");
  info.next_manifest_url = Text(next, "manifest");
  info.content_store_url = Text(root, "content-store");
  info.new_shortrev = Text(next, "name");
  info.new_hash = Text(next, "hash");
  info.changelog_html = ChangelogHtml(root["changelog"].get<picojson::array>());

  // The server's end-to-end updater test sets this; reporting back makes the server end the test.
  if (std::getenv("DOLPHIN_UPDATE_TEST_DONE") != nullptr)
  {
    http.Get(fmt::format("{}/update-test-done/{}", ServerRoot(), CurrentProcessId()));
    return;
  }

  OnUpdateAvailable(info);
}

void AutoUpdateChecker::TriggerUpdate(const AutoUpdateChecker::NewVersionInformation& info,
                                      const AutoUpdateChecker::RestartMode restart_mode)
{
  if (s_updater_launched)
  {
    WARN_LOG_FMT(COMMON, "Auto-update: received a redundant trigger request, ignoring");
    return;
  }

#ifdef HAS_UPDATER_PROGRAM
  std::map<std::string, std::string> flags{
      {"this-manifest-url", info.this_manifest_url},
      {"next-manifest-url", info.next_manifest_url},
      {"content-store-url", info.content_store_url},
      {"parent-pid", std::to_string(CurrentProcessId())},
      {"install-base-path", File::GetExeDirectory()},
      {"log-file", File::GetUserPath(D_LOGS_IDX) + "Updater.log"},
  };
  if (restart_mode == RestartMode::RESTART_AFTER_UPDATE)
    flags.emplace("binary-to-restart", File::GetExePath());

  const std::string command_line = UpdaterCommandLine(flags);
  INFO_LOG_FMT(COMMON, "Updater command line: {}", command_line);

  if (LaunchUpdater(command_line))
    s_updater_launched = true;
#else
  (void)info;
  (void)restart_mode;
#endif
}
