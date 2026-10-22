// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <variant>

#include "Common/Config/Config.h"
#include "Core/SysConf.h"

namespace Config
{
// Settings mirrored from the Wii's SYSCONF file. Byte-sized SYSCONF entries are held as u32
// in the config layers.

// IPL.* entries

extern const Info<bool> SYSCONF_SCREENSAVER;
extern const Info<u32> SYSCONF_LANGUAGE;
extern const Info<u32> SYSCONF_COUNTRY;
extern const Info<bool> SYSCONF_WIDESCREEN;
extern const Info<bool> SYSCONF_PROGRESSIVE_SCAN;
extern const Info<bool> SYSCONF_PAL60;
extern const Info<u32> SYSCONF_SOUND_MODE;

// BT.* entries

extern const Info<u32> SYSCONF_SENSOR_BAR_POSITION;
extern const Info<u32> SYSCONF_SENSOR_BAR_SENSITIVITY;
extern const Info<u32> SYSCONF_SPEAKER_VOLUME;
extern const Info<bool> SYSCONF_WIIMOTE_MOTOR;

struct SYSCONFSetting
{
  std::variant<const Info<u32>*, const Info<bool>*> config_info;
  SysConf::Entry::Type type;
};

extern const std::array<SYSCONFSetting, 11> SYSCONF_SETTINGS;

}  // namespace Config
