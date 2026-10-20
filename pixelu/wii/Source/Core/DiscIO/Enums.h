// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>

#include "Common/CommonTypes.h"

namespace DiscIO
{
// The numeric values below are stored in the game list cache (GameFileCache.cpp). Changing them
// means bumping that cache's revision.

// What kind of title a volume holds.
enum class Platform
{
  GameCubeDisc = 0,
  Triforce,
  WiiDisc,
  WiiWAD,
  ELFOrDOL,
  NumberOfPlatforms,
};

// The country a release was made for, as far as we can tell.
enum class Country
{
  Europe = 0,
  Japan,
  USA,
  Australia,
  France,
  Germany,
  Italy,
  Korea,
  Netherlands,
  Russia,
  Spain,
  Taiwan,
  World,
  Unknown,
  NumberOfCountries,
};

// Console regions. 0-2 are the values the disc header's region field (and the Wii's own
// settings) use; 3 is "region free" on real hardware and doubles as "unknown" here.
enum class Region
{
  NTSC_J = 0,   // Japan, Taiwan; also Korea on the GameCube
  NTSC_U = 1,   // The Americas
  PAL = 2,      // Europe, Oceania
  Unknown = 3,  // Region free, or not known
  NTSC_K = 4,   // Korea (Wii)
  DEV = 5,      // Triforce boards have no region
};

// 0-9 are the Wii's language numbers. PAL GameCube banners number their six languages from 0,
// starting at English (see ToGameCubeLanguage). NTSC GameCube games have a single language.
enum class Language
{
  Japanese = 0,
  English = 1,
  German = 2,
  French = 3,
  Spanish = 4,
  Italian = 5,
  Dutch = 6,
  SimplifiedChinese = 7,   // no retail Wii offers it
  TraditionalChinese = 8,  // no retail Wii offers it
  Korean = 9,
  Unknown,
};

std::string GetName(Country country, bool translate);
std::string GetName(Language language, bool translate);
std::string GetName(Region region, bool translate);

bool IsDisc(Platform volume_type);
bool IsWii(Platform volume_type);
bool IsNTSC(Region region);

int ToGameCubeLanguage(Language language);
Language FromGameCubeLanguage(int language);

Country TypicalCountryForRegion(Region region);
Region SysConfCountryToRegion(u8 country_code);
// The fourth game ID character is only a hint about the region; prefer the region field of a
// disc or TMD when there is one.
Region CountryCodeToRegion(u8 country_code, Platform platform,
                           Region expected_region = Region::Unknown,
                           std::optional<u16> revision = {});
Country CountryCodeToCountry(u8 country_code, Platform platform, Region region = Region::Unknown,
                             std::optional<u16> revision = {});

Region GetSysMenuRegion(u16 title_version);
std::string GetSysMenuVersionString(u16 title_version, bool is_vwii);

const std::string& GetCompanyFromID(const std::string& company_id);
}  // namespace DiscIO
