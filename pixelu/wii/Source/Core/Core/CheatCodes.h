// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Common/IniFile.h"

// Game inis list switched-on and switched-off codes by name, one "$Name" line each, in the
// sections "<section>_Enabled" and "<section>_Disabled". Codes are anything with `name` and
// `enabled` members (AR codes, Gecko codes, patches).

// Sets `enabled` on every code named by a "$Name" line of `section`.
template <typename T>
void ReadEnabledOrDisabled(const Common::IniFile& ini, const std::string& section, bool enabled,
                           std::vector<T>* codes)
{
  std::vector<std::string> names;
  ini.GetLines(section, &names, false);

  for (const std::string& entry : names)
  {
    if (entry.empty() || entry.front() != '$')
      continue;

    const std::string_view name = std::string_view(entry).substr(1);
    for (T& code : *codes)
    {
      if (name == code.name)
        code.enabled = enabled;
    }
  }
}

// Applies the "_Enabled" list, then the "_Disabled" list (so a code in both ends up disabled).
template <typename T>
void ReadEnabledAndDisabled(const Common::IniFile& ini, const std::string& section,
                            std::vector<T>* codes)
{
  ReadEnabledOrDisabled(ini, section + "_Enabled", true, codes);
  ReadEnabledOrDisabled(ini, section + "_Disabled", false, codes);
}
