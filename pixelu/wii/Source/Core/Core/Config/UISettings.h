// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/Config/Config.h"

// UI settings that live in Core because the config saving code (IsSettingSaveable) needs to
// see them. They belong to UICommon logically.

namespace Config
{

// Main system, [General] section.

extern const Info<bool> MAIN_USE_DISCORD_PRESENCE;
extern const Info<bool> MAIN_USE_GAME_COVERS;
extern const Info<bool> MAIN_FOCUSED_HOTKEYS;
extern const Info<bool> MAIN_RECURSIVE_ISO_PATHS;
extern const Info<std::string> MAIN_CURRENT_STATE_PATH;

}  // namespace Config
