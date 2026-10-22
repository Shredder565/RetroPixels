// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Config/DefaultLocale.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Contains.h"
#include "Common/StringUtil.h"
#include "Core/Host.h"
#include "DiscIO/Enums.h"

namespace Config
{
namespace
{
// The host's preferred locales (BCP 47 tags such as "en-US" or "zh-Hant-TW"), asked for once.
const std::vector<std::string>& HostLocales()
{
  static const std::vector<std::string> locales = Host_GetPreferredLocales();
  return locales;
}

// Wii SYSCONF language codes (IPL.LNG): the value is the position in this list. Simplified and
// traditional Chinese (values 7 and 8) are told apart separately.
constexpr std::array<std::pair<std::string_view, DiscIO::Language>, 8> LANGUAGE_CODES{{
    {"ja", DiscIO::Language::Japanese},
    {"en", DiscIO::Language::English},
    {"de", DiscIO::Language::German},
    {"fr", DiscIO::Language::French},
    {"es", DiscIO::Language::Spanish},
    {"it", DiscIO::Language::Italian},
    {"nl", DiscIO::Language::Dutch},
    {"ko", DiscIO::Language::Korean},
}};

DiscIO::Language ChineseVariant(const std::vector<std::string>& subtags)
{
  // An explicit script subtag decides; otherwise mainland China and Singapore write simplified
  // characters and everybody else (Taiwan, Hong Kong, Macau, ...) traditional ones.
  if (Common::Contains(subtags, "Hans"))
    return DiscIO::Language::SimplifiedChinese;
  if (Common::Contains(subtags, "Hant"))
    return DiscIO::Language::TraditionalChinese;
  if (Common::Contains(subtags, "CN") || Common::Contains(subtags, "SG"))
    return DiscIO::Language::SimplifiedChinese;
  return DiscIO::Language::TraditionalChinese;
}

std::optional<DiscIO::Language> LanguageOfLocale(const std::string& locale)
{
  const std::vector<std::string> subtags = SplitString(locale, '-');
  if (subtags.empty())
    return std::nullopt;

  const std::string& primary = subtags.front();
  if (primary == "zh")
    return ChineseVariant(subtags);

  for (const auto& [code, language] : LANGUAGE_CODES)
  {
    if (primary == code)
      return language;
  }
  return std::nullopt;
}

DiscIO::Language PickDefaultLanguage()
{
  for (const std::string& locale : HostLocales())
  {
    if (const auto language = LanguageOfLocale(locale))
      return *language;
  }
  return DiscIO::Language::English;
}

// The first two-letter upper case subtag of a locale is its region ("US" in "en-US").
std::optional<std::string> RegionSubtagOf(const std::string& locale)
{
  for (const std::string& subtag : SplitString(locale, '-'))
  {
    const bool is_region =
        subtag.size() == 2 && Common::IsUpper(subtag[0]) && Common::IsUpper(subtag[1]);
    if (is_region)
      return subtag;
  }
  return std::nullopt;
}

std::string HostCountryCode()
{
#ifdef _WIN32
  // The Windows home location setting is a better guess of where the user is than the UI
  // language. GetUserDefaultGeoName reports the buffer size needed: 3 for a two-letter code.
  const int needed = GetUserDefaultGeoName(nullptr, 0);
  if (needed == 3)
  {
    std::wstring geo_name(static_cast<size_t>(needed), L'\0');
    const int written = GetUserDefaultGeoName(geo_name.data(), needed);
    geo_name.resize(2);
    if (written > 0)
      return WStringToUTF8(geo_name);
  }
#endif

  // Otherwise use the region of the first locale that has one. This can be wrong (plenty of
  // people use en-US anywhere in the world), but it is the best guess available.
  for (const std::string& locale : HostLocales())
  {
    if (auto region = RegionSubtagOf(locale))
      return *region;
  }
  return "";
}

// Wii SYSCONF country codes (IPL.SADR): ISO 3166 codes at their SYSCONF value, two characters
// each. "--" marks values that no country uses.
constexpr std::string_view SYSCONF_COUNTRY_CODES =
    "--JP------------AIAGARAWBSBBBZBO"  // 0-15
    "BRVGCAKYCLCOCRDMDOECSVGFGDGPGTGY"  // 16-31
    "HTHNJMMQMXMSANNIPAPYPEKNLCVCSRTT"  // 32-47
    "TCUSUYVIVE----------------------"  // 48-63
    "ALAUATBEBABWBGHRCYCZDKEEFIFRDEGR"  // 64-79
    "HUISIEITLVLSLILTLUMKMTMEMZNANLNZ"  // 80-95
    "NOPLPTRORURSSKSIZAESSZSECHTRGBZM"  // 96-111
    "ZWAZMRMLNETDSDERDJSO------------"  // 112-127
    "TW--------------KR--------------"  // 128-143
    "HKMO------------IDSGTHPHMY------"  // 144-159
    "CN--------------AEINEGOMQAKWSASY"  // 160-175
    "BHJO";                             // 176-177
static_assert(SYSCONF_COUNTRY_CODES.size() == 178 * 2);

std::optional<u8> PickDefaultCountry()
{
  std::string code = HostCountryCode();

  // The Netherlands Antilles were dissolved after the Wii came out; their successors use the old
  // code.
  if (code == "BQ" || code == "CW" || code == "SX")
    code = "AN";

  if (code.size() != 2)
    return std::nullopt;
  for (size_t value = 0; value * 2 < SYSCONF_COUNTRY_CODES.size(); ++value)
  {
    if (SYSCONF_COUNTRY_CODES.substr(value * 2, 2) == code)
      return static_cast<u8>(value);
  }
  return std::nullopt;
}

// Used when the host's country can't be mapped: Switzerland.
constexpr u8 COUNTRY_IF_UNKNOWN = 0x6c;
}  // namespace

DiscIO::Language GetDefaultLanguage()
{
  static const DiscIO::Language language = PickDefaultLanguage();
  return language;
}

std::optional<u8> GetOptionalDefaultCountry()
{
  static const std::optional<u8> country = PickDefaultCountry();
  return country;
}

u8 GetDefaultCountry()
{
  return GetOptionalDefaultCountry().value_or(COUNTRY_IF_UNKNOWN);
}

DiscIO::Region GetDefaultRegion()
{
  // The region follows the default country, which always exists (falling back to Switzerland),
  // and every country code in the table belongs to a region.
  static const DiscIO::Region region = [] {
    const DiscIO::Region from_country = DiscIO::SysConfCountryToRegion(GetDefaultCountry());
    ASSERT(from_country != DiscIO::Region::Unknown);
    return from_country;
  }();
  return region;
}

}  // namespace Config
