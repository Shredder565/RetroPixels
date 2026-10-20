// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/Enums.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/Common.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

namespace DiscIO
{
namespace
{
// Looks up `value` in a table of names indexed by the enum's numeric value. Anything outside the
// table is "Unknown". The _trans markers let the translation tools find the strings.
template <typename Enum, size_t N>
std::string NameFromTable(const std::array<const char*, N>& names, Enum value, bool translate)
{
  const auto index = static_cast<size_t>(value);
  const char* name = index < N && names[index] ? names[index] : _trans("Unknown");
  return translate ? Common::GetStringT(name) : std::string(name);
}

constexpr std::array<const char*, 13> COUNTRY_NAMES = {
    _trans("Europe"),      _trans("Japan"),  _trans("USA"),   _trans("Australia"),
    _trans("France"),      _trans("Germany"), _trans("Italy"), _trans("Korea"),
    _trans("Netherlands"), _trans("Russia"), _trans("Spain"), _trans("Taiwan"),
    _trans("World"),
};

constexpr std::array<const char*, 10> LANGUAGE_NAMES = {
    _trans("Japanese"),           _trans("English"), _trans("German"), _trans("French"),
    _trans("Spanish"),            _trans("Italian"), _trans("Dutch"),
    _trans("Simplified Chinese"), _trans("Traditional Chinese"),       _trans("Korean"),
};

// Region::Unknown (3) and Region::DEV (5) have no name of their own.
constexpr std::array<const char*, 5> REGION_NAMES = {
    _trans("NTSC-J"), _trans("NTSC-U"), _trans("PAL"), nullptr, _trans("NTSC-K"),
};

// A GameCube game ID ending in E is normally an American release, but Korea got some GameCube
// games in English under E codes too. Those have revision numbers counted from 0x30; without a
// revision, the disc header's region decides.
bool IsKoreanEnglishGameCubeRelease(Platform platform, Region region, std::optional<u16> revision)
{
  if (platform != Platform::GameCubeDisc)
    return false;
  if (revision)
    return *revision >= 0x30;
  return region == Region::NTSC_J;
}

bool IsOneOf(u8 c, std::string_view set)
{
  return c != 0 && set.find(static_cast<char>(c)) != std::string_view::npos;
}
}  // namespace

std::string GetName(Country country, bool translate)
{
  return NameFromTable(COUNTRY_NAMES, country, translate);
}

std::string GetName(Language language, bool translate)
{
  return NameFromTable(LANGUAGE_NAMES, language, translate);
}

std::string GetName(Region region, bool translate)
{
  return NameFromTable(REGION_NAMES, region, translate);
}

bool IsDisc(Platform volume_type)
{
  switch (volume_type)
  {
  case Platform::GameCubeDisc:
  case Platform::Triforce:
  case Platform::WiiDisc:
    return true;
  default:
    return false;
  }
}

bool IsWii(Platform volume_type)
{
  switch (volume_type)
  {
  case Platform::WiiDisc:
  case Platform::WiiWAD:
    return true;
  default:
    return false;
  }
}

bool IsNTSC(Region region)
{
  switch (region)
  {
  case Region::NTSC_J:
  case Region::NTSC_U:
  case Region::NTSC_K:
    return true;
  default:
    return false;
  }
}

// PAL GameCube banners carry English, German, French, Spanish, Italian and Dutch, numbered 0-5,
// which is the Wii numbering minus one.
int ToGameCubeLanguage(Language language)
{
  const bool on_gamecube = language >= Language::English && language <= Language::Dutch;
  return on_gamecube ? static_cast<int>(language) - 1 : 0;
}

Language FromGameCubeLanguage(int language)
{
  return language >= 0 && language <= 5 ? static_cast<Language>(language + 1) :
                                           Language::Unknown;
}

// The game list cache stores the results of the functions below; bump its revision
// (GameFileCache.cpp) when they change.

Country TypicalCountryForRegion(Region region)
{
  switch (region)
  {
  case Region::NTSC_J:
    return Country::Japan;
  case Region::NTSC_U:
    return Country::USA;
  case Region::PAL:
    return Country::Europe;
  case Region::NTSC_K:
    return Country::Korea;
  default:
    return Country::Unknown;
  }
}

// The Wii's SYSCONF country codes are grouped by area: Japan, the Americas, Europe and its
// neighbours, East Asia, the Middle East.
Region SysConfCountryToRegion(u8 country_code)
{
  struct Range
  {
    u8 end;  // exclusive
    Region region;
  };
  static constexpr std::array<Range, 5> ranges = {{
      {0x08, Region::NTSC_J},
      {0x40, Region::NTSC_U},
      {0x80, Region::PAL},
      {0xa8, Region::NTSC_J},
      {0xc0, Region::NTSC_U},
  }};

  if (country_code == 0)
    return Region::Unknown;
  if (country_code == 0x88)  // South Korea, inside the East Asia block
    return Region::NTSC_K;
  for (const Range& range : ranges)
  {
    if (country_code < range.end)
      return range.region;
  }
  return Region::Unknown;
}

Region CountryCodeToRegion(u8 country_code, Platform platform, Region expected_region,
                           std::optional<u16> revision)
{
  if (country_code == 2)  // the Wii Menu uses one title ID for every region
    return expected_region;

  if (country_code == 'E')
  {
    return IsKoreanEnglishGameCubeRelease(platform, expected_region, revision) ? Region::NTSC_J :
                                                                                 Region::NTSC_U;
  }

  // W: Taiwanese Wii releases and Korean GameCube releases in English, except for the one PAL
  // title that uses it (the Nordic Ratatouille for Wii).
  if (country_code == 'W')
    return expected_region == Region::PAL ? Region::PAL : Region::NTSC_J;

  // X, Y, Z: extra language editions, shop exclusives and the like, mostly in Europe.
  if (IsOneOf(country_code, "XYZ"))
    return expected_region == Region::NTSC_U ? Region::NTSC_U : Region::PAL;

  // K, Q, T: Korean releases. The GameCube has no separate Korean region.
  if (IsOneOf(country_code, "KQT"))
    return platform == Platform::GameCubeDisc ? Region::NTSC_J : Region::NTSC_K;

  if (country_code == 'J')
    return Region::NTSC_J;
  if (IsOneOf(country_code, "BN"))
    return Region::NTSC_U;
  if (IsOneOf(country_code, "DFHILMPRSUV"))
    return Region::PAL;

  return Region::Unknown;
}

Country CountryCodeToCountry(u8 country_code, Platform platform, Region region,
                             std::optional<u16> revision)
{
  switch (country_code)
  {
  case 'A':
    return Country::World;

  case 'X':
  case 'Y':
  case 'Z':
    return region == Region::NTSC_U ? Country::USA : Country::Europe;

  case 'W':
    if (platform == Platform::GameCubeDisc)
      return Country::Korea;
    return region == Region::PAL ? Country::Europe : Country::Taiwan;

  case 'E':
    return IsKoreanEnglishGameCubeRelease(platform, region, revision) ? Country::Korea :
                                                                        Country::USA;

  case 'B':  // PAL titles on the American Virtual Console
  case 'N':  // Japanese titles on the American Virtual Console
    return Country::USA;

  case 'J':
    return Country::Japan;

  case 'K':  // Korean releases
  case 'Q':  // Japanese titles on the Korean Virtual Console
  case 'T':  // American titles on the Korean Virtual Console
    return Country::Korea;

  case 'P':  // the usual European code
  case 'L':  // Japanese titles on the European Virtual Console
  case 'M':  // American titles on the European Virtual Console
  case 'V':  // some Nordic Wii releases
    return Country::Europe;

  case 'D':
    return Country::Germany;
  case 'F':
    return Country::France;
  case 'H':
    return Country::Netherlands;
  case 'I':
    return Country::Italy;
  case 'R':
    return Country::Russia;
  case 'S':
    return Country::Spain;
  case 'U':
    return Country::Australia;

  default:
    // IOS WADs and the like have small numbers here; only warn about letters.
    if (country_code > 'A')
      WARN_LOG_FMT(DISCIO, "Unknown Country Code! {}", static_cast<char>(country_code));
    return Country::Unknown;
  }
}

// The low nibble of a Wii Menu title version says which region it is for.
Region GetSysMenuRegion(u16 title_version)
{
  switch (title_version & 0xf)
  {
  case 0:
    return Region::NTSC_J;
  case 1:
    return Region::NTSC_U;
  case 2:
    return Region::PAL;
  case 6:
    return Region::NTSC_K;
  default:
    return Region::Unknown;
  }
}

std::string GetSysMenuVersionString(u16 title_version, bool is_vwii)
{
  struct Release
  {
    u16 base;  // title version with the region nibble cleared
    const char* name;
  };
  // On a Wii U, the menu version is named after the Wii U system release that installed it.
  static constexpr std::array<Release, 3> vwii_releases = {{
      {512, "1.0.0"},
      {544, "4.0.0"},
      {608, "5.2.0"},
  }};
  static constexpr std::array<Release, 15> wii_releases = {{
      {32, "1.0"},
      {96, "2.0"},
      {128, "2.0"},
      {160, "2.1"},
      {192, "2.2"},
      {224, "3.0"},
      {256, "3.1"},
      {288, "3.2"},
      {320, "3.3"},
      {352, "3.3"},
      {384, "3.4"},  // 3.5 in Korea, see below
      {416, "4.0"},
      {448, "4.1"},
      {480, "4.2"},
      {512, "4.3"},
  }};

  char region_letter = 0;
  switch (GetSysMenuRegion(title_version))
  {
  case Region::NTSC_J:
    region_letter = 'J';
    break;
  case Region::NTSC_U:
    region_letter = 'U';
    break;
  case Region::PAL:
    region_letter = 'E';
    break;
  case Region::NTSC_K:
    region_letter = 'K';
    break;
  default:
    WARN_LOG_FMT(DISCIO, "Unknown region for Wii Menu version {}", title_version);
    break;
  }

  const u16 base = title_version & 0xff0;
  const std::span<const Release> releases =
      is_vwii ? std::span<const Release>(vwii_releases) : std::span<const Release>(wii_releases);
  const auto release = std::ranges::find(releases, base, &Release::base);

  std::string version = release != releases.end() ? release->name : (is_vwii ? "?.?.?" : "?.?");
  // Korea skipped 3.4; its first menu with this version number was released as 3.5.
  if (!is_vwii && base == 384 && region_letter == 'K')
    version = "3.5";

  if (region_letter != 0)
    version += region_letter;
  return version;
}

namespace
{
struct Company
{
  const char* code;
  const char* name;
};

// Two-character maker codes (from game IDs) and the publishers they belong to, sorted by code.
// Non-ASCII names are spelled out as UTF-8 bytes, since u8"" literals are char8_t in C++20.
constexpr Company COMPANIES[] = {
    {"01", "Nintendo"},
    {"02", "Nintendo"},
    {"08", "Capcom"},
    {"0A", "Jaleco / Jaleco Entertainment"},
    {"0L", "Warashi"},
    {"0M", "Entertainment Software Publishing"},
    {"0Q", "IE Institute"},
    {"13", "Electronic Arts Japan"},
    {"18", "Hudson Soft / Hudson Entertainment"},
    {"1K", "Titus Software"},
    {"20", "DSI Games / ZOO Digital Publishing"},
    {"28", "Kemco Japan"},
    {"29", "SETA Corporation"},
    {"2K", "NEC Interchannel"},
    {"2L", "Agatsuma Entertainment"},
    {"2M", "Jorudan"},
    {"2N", "Smilesoft / Rocket Company"},
    {"2Q", "MediaKite"},
    {"36", "Codemasters"},
    {"41", "Ubisoft"},
    {"4F", "Eidos Interactive"},
    {"4Q", "Disney Interactive Studios / Buena Vista Games"},
    {"4Z", "Crave Entertainment / Red Wagon Games"},
    {"51", "Acclaim Entertainment"},
    {"52", "Activision"},
    {"54", "Take-Two Interactive / GameTek / Rockstar Games / Global Star Software"},
    {"5D", "Midway Games / Tradewest"},
    {"5G", "Majesco Entertainment"},
    {"5H", "3DO / Global Star Software"},
    {"5L", "NewKidCo"},
    {"5S", "Evolved Games / Xicat Interactive"},
    {"5V", "Agetec"},
    {"5Z", "Data Design / Conspiracy Entertainment"},
    {"60", "Titus Interactive / Titus Software"},
    {"64", "LucasArts"},
    {"68", "Bethesda Softworks / Mud Duck Productions / Vir2L Studios"},
    {"69", "Electronic Arts"},
    {"6E", "Sega"},
    {"6K", "UFO Interactive Games"},
    {"6L", "BAM! Entertainment"},
    {"6M", "System 3"},
    {"6N", "Midas Interactive Entertainment"},
    {"6S", "TDK Mediactive"},
    {"6U", "The Adventure Company / DreamCatcher Interactive"},
    {"6V", "JoWooD Entertainment"},
    {"6W", "Sega"},
    {"6X", "Wanadoo Edition"},
    {"6Z", "NDS Software"},
    {"70", "Atari (Infogrames)"},
    {"71", "Interplay Entertainment"},
    {"75", "SCi Games"},
    {"78", "THQ / Play THQ"},
    {"7D", "Sierra Entertainment / Vivendi Games / Universal Interactive Studios"},
    {"7F", "Kemco"},
    {"7G", "Rage Software"},
    {"7H", "Encore Software"},
    {"7J", "Zushi Games / ZOO Digital Publishing"},
    {"7K", "Kiddinx Entertainment"},
    {"7L", "Simon & Schuster Interactive"},
    {"7M", "Badland Games"},
    {"7N", "Empire Interactive / Xplosiv"},
    {"7S", "Rockstar Games"},
    {"7T", "Scholastic"},
    {"7U", "Ignition Entertainment"},
    {"82", "Namco"},
    {"8G", "NEC Interchannel"},
    {"8J", "Kadokawa Shoten"},
    {"8M", "CyberFront"},
    {"8N", "Success"},
    {"8P", "Sega"},
    {"91", "Chunsoft"},
    {"99", "Marvelous Entertainment / Victor Entertainment / Pack-In-Video / Rising Star Games"},
    {"9B", "Tecmo"},
    {"9G", "Take-Two Interactive / Global Star Software / Gotham Games / Gathering of Developers"},
    {"9S", "Brother International"},
    {"9Z", "Crunchyroll"},
    {"A4", "Konami"},
    {"A7", "Takara"},
    {"AF", "Namco Bandai Games"},
    {"AU", "Alternative Software"},
    {"AX", "Vivendi"},
    {"B0", "Acclaim Japan"},
    {"B2", "Bandai Games"},
    {"BB", "Sunsoft"},
    {"BL", "MTO"},
    {"BM", "XING"},
    {"BN", "Sunrise Interactive"},
    {"BP", "Global A Entertainment"},
    {"C0", "Taito"},
    {"C8", "Koei"},
    {"CM", "Konami Computer Entertainment Osaka"},
    {"CQ", "From Software"},
    {"D9", "Banpresto"},
    {"DA", "Tomy / Takara Tomy"},
    {"DQ", "Compile Heart / Idea Factory"},
    {"E5", "Epoch"},
    {"E6", "Game Arts"},
    {"E7", "Athena"},
    {"E8", "Asmik Ace Entertainment"},
    {"E9", "Natsume"},
    {"EB", "Atlus"},
    {"EL", "Spike"},
    {"EM", "Konami Computer Entertainment Tokyo"},
    {"EP", "Sting Entertainment"},
    {"ES", "Starfish-SD"},
    {"EY", "Vblank Entertainment"},
    {"FH", "Easy Interactive"},
    {"FJ", "Virtual Toys"},
    {"FK", "The Game Factory"},
    {"FP", "Mastiff"},
    {"FR", "Digital Tainment Pool"},
    {"FS", "XS Games"},
    {"G0", "Alpha Unit"},
    {"G2", "Yuke's"},
    {"G6", "SIMS"},
    {"G9", "D3 Publisher"},
    {"GA", "PIN Change"},
    {"GD", "Square Enix"},
    {"GE", "Kids Station"},
    {"GG", "O3 Entertainment"},
    {"GJ", "Detn8 Games"},
    {"GK", "Genki"},
    {"GL", "Gameloft / Ubisoft"},
    {"GM", "Gamecock Media Group"},
    {"GN", "Oxygen Games"},
    {"GR", "GSP"},
    {"GT", "505 Games"},
    {"GX", "Commodore"},
    {"GY", "The Game Factory"},
    {"GZ", "Gammick Entertainment"},
    {"H3", "Zen United"},
    {"H4", "SNK Playmore"},
    {"HA", "Nobilis"},
    {"HE", "Gust"},
    {"HF", "Level-5"},
    {"HG", "Graffiti Entertainment"},
    {"HH", "Focus Home Interactive"},
    {"HJ", "Genius Products"},
    {"HK", "D2C Games"},
    {"HL", "Frontier Developments"},
    {"HM", "HMH Interactive"},
    {"HN", "High Voltage Software"},
    {"HQ", "Abstraction Games"},
    {"HS", "Tru Blu"},
    {"HT", "Big Blue Bubble"},
    {"HU", "Ghostfire Games"},
    {"HW", "Incredible Technologies"},
    {"HY", "Reef Entertainment"},
    {"HZ", "Nordcurrent"},
    {"J8", "D4 Enterprise"},
    {"J9", "AQ Interactive"},
    {"JD", "SKONEC Entertainment"},
    {"JE", "E Frontier"},
    {"JF", "Arc System Works"},
    {"JG", "The Games Company"},
    {"JH", "City Interactive"},
    {"JJ", "Deep Silver"},
    {"JP", "redspotgames"},
    {"JR", "Engine Software"},
    {"JS", "Digital Leisure"},
    {"JT", "Empty Clip Studios"},
    {"JU", "Riverman Media"},
    {"JV", "JV Games"},
    {"JW", "BigBen Interactive"},
    {"JX", "Shin'en Multimedia"},
    {"JY", "Steel Penny Games"},
    {"JZ", "505 Games"},
    {"K2", "Coca-Cola (Japan) Company"},
    {"K3", "Yudo"},
    {"K6", "Nihon System"},
    {"KB", "Nippon Ichi Software"},
    {"KG", "Kando Games"},
    {"KH", "Joju Games"},
    {"KJ", "Studio Zan"},
    {"KK", "DK Games"},
    {"KL", "Abylight"},
    {"KM", "Deep Silver"},
    {"KN", "Gameshastra"},
    {"KP", "Purple Hills"},
    {"KQ", "Over the Top Games"},
    {"KR", "KREA Medie"},
    {"KT", "The Code Monkeys"},
    {"KW", "Semnat Studios"},
    {"KY", "Medaverse Studios"},
    {"L3", "G-Mode"},
    {"L8", "FujiSoft"},
    {"LB", "Tryfirst"},
    {"LD", "Studio Zan"},
    {"LF", "Kemco"},
    {"LG", "Black Bean Games"},
    {"LJ", "Legendo Entertainment"},
    {"LL", "HB Studios"},
    {"LN", "GameOn"},
    {"LP", "Left Field Productions"},
    {"LR", "Koch Media"},
    {"LT", "Legacy Interactive"},
    {"LU", "Lexis Num\xc3\xa9rique"},
    {"LW", "Grendel Games"},
    {"LY", "Icon Games / Super Icon"},
    {"M0", "Studio Zan"},
    {"M1", "Grand Prix Games"},
    {"M2", "HomeMedia"},
    {"M4", "Cybird"},
    {"M6", "Perpetuum"},
    {"MB", "Agenda"},
    {"MD", "Ateam"},
    {"ME", "Silver Star Japan"},
    {"MF", "Yamasa"},
    {"MH", "Mentor Interactive"},
    {"MJ", "Mumbo Jumbo"},
    {"ML", "DTP Young Entertainment"},
    {"MM", "Big John Games"},
    {"MN", "Mindscape"},
    {"MR", "Mindscape"},
    {"MS", "Milestone / UFO Interactive Games"},
    {"MT", "Blast! Entertainment"},
    {"MV", "Marvelous Entertainment"},
    {"MZ", "Mad Catz"},
    {"N0", "Exkee"},
    {"N4", "Zoom"},
    {"N7", "T&S"},
    {"N9", "Tera Box"},
    {"NA", "Tom Create"},
    {"NB", "HI Games & Publishing"},
    {"NE", "Kosaido"},
    {"NF", "Peakvox"},
    {"NG", "Nordic Games"},
    {"NH", "Gevo Entertainment"},
    {"NJ", "Enjoy Gaming"},
    {"NK", "Neko Entertainment"},
    {"NL", "Nordic Softsales"},
    {"NN", "Nnooo"},
    {"NP", "Nobilis"},
    {"NQ", "Namco Bandai Partners"},
    {"NR", "Destineer Publishing / Bold Games"},
    {"NS", "Nippon Ichi Software America"},
    {"NT", "Nocturnal Entertainment"},
    {"NV", "Nicalis"},
    {"NW", "Deep Fried Entertainment"},
    {"NX", "Barnstorm Games"},
    {"NY", "Nicalis"},
    {"P1", "Poisoft"},
    {"PH", "Playful Entertainment"},
    {"PK", "Knowledge Adventure"},
    {"PL", "Playlogic Entertainment"},
    {"PM", "Warner Bros. Interactive Entertainment"},
    {"PN", "P2 Games"},
    {"PQ", "PopCap Games"},
    {"PS", "Bplus"},
    {"PT", "Firemint"},
    {"PU", "Pub Company"},
    {"PV", "Pan Vision"},
    {"PY", "Playstos Entertainment"},
    {"PZ", "GameMill Publishing"},
    {"Q2", "Santa Entertainment"},
    {"Q3", "Asterizm"},
    {"Q4", "Hamster"},
    {"Q5", "Recom"},
    {"QA", "Miracle Kidz"},
    {"QC", "Kadokawa Shoten / Enterbrain"},
    {"QH", "Virtual Play Games"},
    {"QK", "MACHINE Studios"},
    {"QM", "Object Vision Software"},
    {"QQ", "Gamelion"},
    {"QR", "Lapland Studio"},
    {"QT", "CALARIS"},
    {"QU", "QubicGames"},
    {"QV", "Ludia"},
    {"QW", "Kaasa Solution"},
    {"QX", "Press Play"},
    {"QZ", "Hands-On Mobile"},
    {"RA", "Office Create"},
    {"RG", "Ronimo Games"},
    {"RH", "h2f Games"},
    {"RM", "Rondomedia"},
    {"RN", "Mastiff / N3V Games"},
    {"RQ", "GolemLabs & Zoozen"},
    {"RS", "Brash Entertainment"},
    {"RT", "RTL Enterprises"},
    {"RV", "bitComposer Games"},
    {"RW", "RealArcade"},
    {"RX", "Reflexive Entertainment"},
    {"RZ", "Akaoni Studio"},
    {"S5", "SouthPeak Games"},
    {"SH", "Sabarasa"},
    {"SJ", "Cosmonaut Games"},
    {"SP", "Blade Interactive Studios"},
    {"SQ", "Sonalysts"},
    {"SR", "SnapDragon Games"},
    {"SS", "Sanuk Games"},
    {"ST", "Stickmen Studios"},
    {"SU", "Slitherine"},
    {"SV", "SevenOne Intermedia"},
    {"SZ", "Storm City Games"},
    {"TH", "Kolkom"},
    {"TJ", "Broken Rules"},
    {"TL", "Telltale Games"},
    {"TR", "Tetris Online"},
    {"TS", "Triangle Studios"},
    {"TV", "Tivola"},
    {"TW", "Two Tribes"},
    {"TY", "Teyon"},
    {"UG", "Data Design Interactive / Popcorn Arcade / Metro 3D"},
    {"UH", "Intenium Console"},
    {"UJ", "Ghostlight"},
    {"UK", "iFun4all"},
    {"UN", "Chillingo"},
    {"UP", "EnjoyUp Games"},
    {"UR", "Sudden Games"},
    {"US", "USM"},
    {"UU", "Onteca"},
    {"UV", "Fugazo"},
    {"UW", "Coresoft"},
    {"VG", "Vogster Entertainment"},
    {"VK", "Sandlot Games"},
    {"VL", "Eko Software"},
    {"VN", "Valcon Games"},
    {"VP", "Virgin Play"},
    {"VS", "Korner Entertainment"},
    {"VT", "Microforum Games"},
    {"VU", "Double Jungle"},
    {"VV", "Pixonauts"},
    {"VX", "Frontline Studios"},
    {"VZ", "Little Orbit"},
    {"WD", "Amazon"},
    {"WG", "2D Boy"},
    {"WH", "NinjaBee"},
    {"WJ", "Studio Walljump"},
    {"WL", "Wired Productions"},
    {"WN", "tons of bits"},
    {"WP", "White Park Bay Software"},
    {"WQ", "Revistronic"},
    {"WR", "Warner Bros. Interactive Entertainment"},
    {"WS", "MonkeyPaw Games"},
    {"WW", "Slang Publishing"},
    {"WY", "WayForward Technologies"},
    {"WZ", "Wizarbox"},
    {"X0", "SDP Games"},
    {"X3", "CK Games"},
    {"X4", "Easy Interactive"},
    {"XB", "Hulu"},
    {"XG", "XGen Studios"},
    {"XJ", "XSEED Games"},
    {"XK", "Exkee"},
    {"XM", "DreamBox Games"},
    {"XN", "Netflix"},
    {"XS", "Aksys Games"},
    {"XT", "Funbox Media"},
    {"XU", "Shanblue Interactive"},
    {"XV", "Keystone Game Studio"},
    {"XW", "Lemon Games"},
    {"XY", "Gaijin Games"},
    {"Y1", "Tubby Games"},
    {"Y5", "Easy Interactive"},
    {"Y6", "Motiviti"},
    {"Y7", "The Learning Company"},
    {"Y9", "RadiationBurn"},
    {"YC", "NECA"},
    {"YD", "Infinite Dreams"},
    {"YF", "O2 Games"},
    {"YG", "Maximum Family Games"},
    {"YJ", "Frozen Codebase"},
    {"YK", "MAD Multimedia"},
    {"YN", "Game Factory"},
    {"YS", "Yullaby"},
    {"YT", "Corecell Technology"},
    {"YV", "KnapNok Games"},
    {"YX", "Selectsoft"},
    {"YY", "FDG Entertainment"},
    {"Z4", "Ntreev Soft"},
    {"Z5", "Shinsegae I&C"},
    {"ZA", "WBA Interactive"},
    {"ZG", "Zallag"},
    {"ZH", "Internal Engine"},
    {"ZJ", "Performance Designed Products"},
    {"ZK", "Anima Game Studio"},
    {"ZP", "Fishing Cactus"},
    {"ZS", "Zinkia Entertainment"},
    {"ZV", "RedLynx"},
    {"ZW", "Judo Baby"},
    {"ZX", "TopWare Interactive"},
};
}  // namespace

const std::string& GetCompanyFromID(const std::string& company_id)
{
  struct Entry
  {
    std::string code;
    std::string name;
  };
  static const std::vector<Entry> table = [] {
    std::vector<Entry> entries;
    entries.reserve(std::size(COMPANIES));
    for (const Company& company : COMPANIES)
      entries.push_back({company.code, company.name});
    return entries;
  }();
  static const std::string no_company;

  const auto it = std::ranges::lower_bound(table, company_id, {}, &Entry::code);
  return it != table.end() && it->code == company_id ? it->name : no_company;
}
}  // namespace DiscIO
