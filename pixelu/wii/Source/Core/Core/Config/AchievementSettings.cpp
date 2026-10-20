// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef USE_RETRO_ACHIEVEMENTS
#include "Core/Config/AchievementSettings.h"

#include <string>

namespace Config
{
namespace
{
Location AchievementsKey(std::string_view key)
{
  return Location{System::Achievements, "Achievements", std::string(key)};
}
}  // namespace

// Account and server.
const Info<bool> RA_ENABLED{AchievementsKey("Enabled"), false};
const Info<std::string> RA_HOST_URL{AchievementsKey("HostUrl"), ""};
const Info<std::string> RA_USERNAME{AchievementsKey("Username"), ""};
const Info<std::string> RA_API_TOKEN{AchievementsKey("ApiToken"), ""};

// Game modes. Hardcore is the default once achievements are switched on.
const Info<bool> RA_HARDCORE_ENABLED{AchievementsKey("HardcoreEnabled"), true};
const Info<bool> RA_UNOFFICIAL_ENABLED{AchievementsKey("UnofficialEnabled"), false};
const Info<bool> RA_ENCORE_ENABLED{AchievementsKey("EncoreEnabled"), false};
const Info<bool> RA_SPECTATOR_ENABLED{AchievementsKey("SpectatorEnabled"), false};

// On-screen elements and integrations.
const Info<bool> RA_LEADERBOARD_TRACKER_ENABLED{AchievementsKey("LeaderboardTrackerEnabled"), true};
const Info<bool> RA_CHALLENGE_INDICATORS_ENABLED{AchievementsKey("ChallengeIndicatorsEnabled"),
                                                 true};
const Info<bool> RA_DISCORD_PRESENCE_ENABLED{AchievementsKey("DiscordPresenceEnabled"), false};
const Info<bool> RA_PROGRESS_ENABLED{AchievementsKey("ProgressEnabled"), false};
}  // namespace Config

#endif  // USE_RETRO_ACHIEVEMENTS
