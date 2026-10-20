// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/WC24PatchEngine.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "Common/Contains.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"

#include "Core/CheatCodes.h"
#include "Core/CommonTitles.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"

namespace WC24PatchEngine
{
namespace
{
// Only these titles get patched.
constexpr std::array<u64, 15> kPatchableTitles{
    Titles::NINTENDO_CHANNEL_NTSC_U,        Titles::NINTENDO_CHANNEL_NTSC_J,
    Titles::NINTENDO_CHANNEL_PAL,           Titles::FORECAST_CHANNEL_NTSC_U,
    Titles::FORECAST_CHANNEL_NTSC_J,        Titles::FORECAST_CHANNEL_PAL,
    Titles::NEWS_CHANNEL_NTSC_U,            Titles::NEWS_CHANNEL_NTSC_J,
    Titles::NEWS_CHANNEL_PAL,               Titles::EVERYBODY_VOTES_CHANNEL_NTSC_U,
    Titles::EVERYBODY_VOTES_CHANNEL_NTSC_J, Titles::EVERYBODY_VOTES_CHANNEL_PAL,
    Titles::REGION_SELECT_CHANNEL_NTSC_U,   Titles::REGION_SELECT_CHANNEL_NTSC_J,
    Titles::REGION_SELECT_CHANNEL_PAL,
};

constexpr std::string_view kHostHeader = "Host: ";

std::vector<NetworkPatch> s_patches;

// Section lines: "$Name" names the following patches; "source:replacement:is_kd" adds a patch
// under the current name.
void ReadSection(const Common::IniFile& ini)
{
  std::vector<std::string> lines;
  ini.GetLines("WC24Patch", &lines);

  std::string name;
  for (const std::string& line : lines)
  {
    if (line.empty())
      continue;
    if (line[0] == '$')
    {
      name = line.substr(1);
      continue;
    }

    const std::vector<std::string> fields = SplitString(line, ':');
    if (fields.size() != 3)
      continue;

    NetworkPatch patch;
    patch.name = name;
    patch.source = fields[0];
    patch.replacement = fields[1];
    if (TryParse(fields[2], &patch.is_kd))
      s_patches.push_back(std::move(patch));
  }

  ReadEnabledAndDisabled(ini, "WC24Patch", &s_patches);
}
}  // namespace

void Reload()
{
  s_patches.clear();

  const SConfig& config = SConfig::GetInstance();
  if (!Common::Contains(kPatchableTitles, config.GetTitleID()))
    return;

  // The shipped (default) ini holds the WiiLink addresses; without WiiLink the user's own ini is
  // used.
  const Common::IniFile ini = Config::Get(Config::MAIN_WII_WIILINK_ENABLE) ?
                                  config.LoadDefaultGameIni() :
                                  config.LoadLocalGameIni();
  ReadSection(ini);
}

std::optional<std::string> GetNetworkPatch(std::string_view source, IsKD is_kd)
{
  for (const NetworkPatch& patch : s_patches)
  {
    if (patch.enabled && patch.is_kd == is_kd && patch.source == source)
      return patch.replacement;
  }
  return std::nullopt;
}

std::optional<std::string> GetNetworkPatchByPayload(std::string_view source)
{
  // Sockets hand over the whole request, so the host is found in its first "Host: " line (lines
  // end with CRLF; the last line may not).
  std::size_t line_start = 0;
  while (line_start < source.size())
  {
    const std::size_t line_end = source.find("\r\n", line_start);
    const std::string_view line = source.substr(line_start, line_end == std::string_view::npos ?
                                                                std::string_view::npos :
                                                                line_end - line_start);
    if (line.starts_with(kHostHeader))
    {
      const std::string_view host = line.substr(kHostHeader.size());
      const auto match = std::ranges::find_if(s_patches, [&](const NetworkPatch& patch) {
        return patch.enabled && patch.is_kd != IsKD{true} && patch.source == host;
      });
      if (match == s_patches.end())
        return std::nullopt;

      const std::string_view rest =
          line_end == std::string_view::npos ? std::string_view{} : source.substr(line_end);
      return fmt::format("{}{}{}{}", source.substr(0, line_start), kHostHeader, match->replacement,
                         rest);
    }

    if (line_end == std::string_view::npos)
      break;
    line_start = line_end + 2;
  }
  return std::nullopt;
}
}  // namespace WC24PatchEngine
