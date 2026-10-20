// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Config/UISettings.h"

namespace Config
{
namespace
{
Location GeneralKey(std::string_view key)
{
  return Location{System::Main, "General", std::string(key)};
}
}  // namespace

const Info<bool> MAIN_USE_DISCORD_PRESENCE{GeneralKey("UseDiscordPresence"), true};
// Cover art downloads are opt-in on desktop builds.
const Info<bool> MAIN_USE_GAME_COVERS{GeneralKey("UseGameCovers"), false};
const Info<bool> MAIN_FOCUSED_HOTKEYS{GeneralKey("HotkeysRequireFocus"), true};
const Info<bool> MAIN_RECURSIVE_ISO_PATHS{GeneralKey("RecursiveISOPaths"), false};
const Info<std::string> MAIN_CURRENT_STATE_PATH{GeneralKey("CurrentStatePath"), ""};

}  // namespace Config
