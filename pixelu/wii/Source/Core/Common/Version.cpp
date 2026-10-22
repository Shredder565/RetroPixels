// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Version.h"

#include <initializer_list>
#include <string>
#include <string_view>

#include "Common/scmrev.h"

namespace Common
{
namespace
{
// The emulator name is part of the user agent, the window title and the netplay handshake, so it
// stays what existing servers and peers expect.
constexpr std::string_view EMULATOR_NAME = "Dolphin";
// The program named by --version, savestates and shader reports.
constexpr std::string_view PRODUCT_NAME = "PixelU-Wii";

#if defined(_DEBUG)
constexpr std::string_view BUILD_TYPE_PREFIX = "Debug ";
#elif defined(DEBUGFAST)
constexpr std::string_view BUILD_TYPE_PREFIX = "DebugFast ";
#else
constexpr std::string_view BUILD_TYPE_PREFIX = "";
#endif

#ifdef _WIN32
constexpr std::string_view NETPLAY_PLATFORM_SUFFIX = " Windows";
#elif defined(__linux__)
constexpr std::string_view NETPLAY_PLATFORM_SUFFIX = " Linux";
#else
constexpr std::string_view NETPLAY_PLATFORM_SUFFIX = "";
#endif

#define PIXELU_VERSION_STRINGIZE_IMPL(x) #x
#define PIXELU_VERSION_STRINGIZE(x) PIXELU_VERSION_STRINGIZE_IMPL(x)
// The build system leaves this empty when it could not find the master branch.
constexpr std::string_view COMMITS_AHEAD_TEXT = PIXELU_VERSION_STRINGIZE(SCM_COMMITS_AHEAD_MASTER);
#undef PIXELU_VERSION_STRINGIZE
#undef PIXELU_VERSION_STRINGIZE_IMPL

std::string Concat(std::initializer_list<std::string_view> parts)
{
  std::string result;
  for (const std::string_view part : parts)
    result += part;
  return result;
}

std::string BuildRevisionString()
{
  // Builds that are not exactly on master show their branch.
  const bool on_master = COMMITS_AHEAD_TEXT == "0";
  std::string branch_part;
  if (!on_master)
    branch_part = Concat({"[", SCM_BRANCH_STR, "] "});
  return Concat({PRODUCT_NAME, " ", branch_part, BUILD_TYPE_PREFIX, SCM_DESC_STR});
}
}  // namespace

const std::string& GetEmulatorName()
{
  static const std::string value(EMULATOR_NAME);
  return value;
}

const std::string& GetScmRevStr()
{
  static const std::string value = BuildRevisionString();
  return value;
}

const std::string& GetScmRevGitStr()
{
  static const std::string value = SCM_REV_STR;
  return value;
}

const std::string& GetScmDescStr()
{
  static const std::string value = SCM_DESC_STR;
  return value;
}

const std::string& GetScmBranchStr()
{
  static const std::string value = SCM_BRANCH_STR;
  return value;
}

const std::string& GetUserAgentStr()
{
  // Servers that check the client's version (RetroAchievements) know only Dolphin's numbers.
  static const std::string value = Concat({EMULATOR_NAME, "/", SCM_COMPAT_DESC_STR});
  return value;
}

const std::string& GetCompatDescStr()
{
  static const std::string value = SCM_COMPAT_DESC_STR;
  return value;
}

const std::string& GetScmDistributorStr()
{
  static const std::string value = SCM_DISTRIBUTOR_STR;
  return value;
}

const std::string& GetScmUpdateTrackStr()
{
  static const std::string value = SCM_UPDATE_TRACK_STR;
  return value;
}

const std::string& GetNetplayDolphinVer()
{
  static const std::string value = Concat({SCM_DESC_STR, NETPLAY_PLATFORM_SUFFIX});
  return value;
}

int GetScmCommitsAheadMaster()
{
  // "+ 0" turns an empty macro into 0.
  return SCM_COMMITS_AHEAD_MASTER + 0;
}
}  // namespace Common
