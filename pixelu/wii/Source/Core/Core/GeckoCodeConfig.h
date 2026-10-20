// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Core/GeckoCode.h"

namespace Common
{
class IniFile;
}

namespace Gecko
{
// The [Gecko] sections of the global (default) and local (user) game inis, in that order.
std::vector<GeckoCode> LoadCodes(const Common::IniFile& globalIni, const Common::IniFile& localIni);
// Fetches a game's code list from the online code database. The error is the HTTP response code.
std::expected<std::vector<GeckoCode>, int> DownloadCodes(std::string_view gametdb_id);
// Writes the user's codes and the enabled/disabled lists to [Gecko], [Gecko_Enabled] and
// [Gecko_Disabled].
void SaveCodes(Common::IniFile& inifile, std::span<const GeckoCode> gcodes);

// Parses an "XXXXXXXX YYYYYYYY" code line.
std::optional<GeckoCode::Code> DeserializeLine(const std::string& line);
}  // namespace Gecko
