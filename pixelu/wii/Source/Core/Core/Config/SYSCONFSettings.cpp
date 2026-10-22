// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Config/SYSCONFSettings.h"

#include "Core/Config/DefaultLocale.h"

namespace Config
{
namespace
{
// The layer location of a SYSCONF entry is its "<group>.<name>" name split in two.
Location SysconfKey(std::string_view group, std::string_view name)
{
  return Location{System::SYSCONF, std::string(group), std::string(name)};
}
}  // namespace

// IPL group: screen saver, language, country, display and sound options.
const Info<bool> SYSCONF_SCREENSAVER{SysconfKey("IPL", "SSV"), false};
const Info<u32> SYSCONF_LANGUAGE{SysconfKey("IPL", "LNG"), static_cast<u32>(GetDefaultLanguage())};
const Info<u32> SYSCONF_COUNTRY{SysconfKey("IPL", "SADR"), GetDefaultCountry()};
const Info<bool> SYSCONF_WIDESCREEN{SysconfKey("IPL", "AR"), true};
const Info<bool> SYSCONF_PROGRESSIVE_SCAN{SysconfKey("IPL", "PGS"), true};
const Info<bool> SYSCONF_PAL60{SysconfKey("IPL", "E60"), true};
const Info<u32> SYSCONF_SOUND_MODE{SysconfKey("IPL", "SND"), 0x01};  // stereo

// BT group: sensor bar, Wii Remote speaker and rumble.
const Info<u32> SYSCONF_SENSOR_BAR_POSITION{SysconfKey("BT", "BAR"), 0x01};  // above the screen
const Info<u32> SYSCONF_SENSOR_BAR_SENSITIVITY{SysconfKey("BT", "SENS"), 0x03};
const Info<u32> SYSCONF_SPEAKER_VOLUME{SysconfKey("BT", "SPKV"), 0x58};
const Info<bool> SYSCONF_WIIMOTE_MOTOR{SysconfKey("BT", "MOT"), true};

using Stored = SysConf::Entry::Type;

// How each setting is stored in the SYSCONF file. The order is part of the NetPlay protocol (the
// host sends the values in this order), so new entries go at the end.
const std::array<SYSCONFSetting, 11> SYSCONF_SETTINGS{{
    {&SYSCONF_SCREENSAVER, Stored::Byte},
    {&SYSCONF_LANGUAGE, Stored::Byte},
    {&SYSCONF_COUNTRY, Stored::BigArray},
    {&SYSCONF_WIDESCREEN, Stored::Byte},
    {&SYSCONF_PROGRESSIVE_SCAN, Stored::Byte},
    {&SYSCONF_PAL60, Stored::Byte},
    {&SYSCONF_SOUND_MODE, Stored::Byte},
    {&SYSCONF_SENSOR_BAR_POSITION, Stored::Byte},
    {&SYSCONF_SENSOR_BAR_SENSITIVITY, Stored::Long},
    {&SYSCONF_SPEAKER_VOLUME, Stored::Byte},
    {&SYSCONF_WIIMOTE_MOTOR, Stored::Byte},
}};
}  // namespace Config
