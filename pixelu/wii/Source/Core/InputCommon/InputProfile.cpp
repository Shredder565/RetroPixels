// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/InputProfile.h"

#include <algorithm>
#include <iterator>

#include <fmt/format.h>

#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Wiimote.h"

#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputConfig.h"

namespace InputProfile
{
namespace
{
constexpr int MESSAGE_MS = 3000;

std::vector<std::string> FindAllProfiles(const InputConfig& config)
{
  return Common::DoFileSearch(config.GetUserProfileDirectoryPath(), ".ini", true);
}

// Moves the index one step and wraps it into the list.
const std::string& StepThrough(const std::vector<std::string>& profiles, int& index,
                               CycleDirection direction)
{
  const int count = static_cast<int>(profiles.size());
  index += static_cast<int>(direction);
  index = ((index % count) + count) % count;
  return profiles[static_cast<std::size_t>(index)];
}

void LoadProfileInto(const std::string& path, ControllerEmu::EmulatedController& controller,
                     InputConfig& config)
{
  std::string profile_name;
  SplitPath(path, nullptr, &profile_name, nullptr);

  Common::IniFile ini;
  if (!ini.Load(path))
  {
    Core::DisplayMessage("Unable to load input profile '" + profile_name + "' for device '" +
                             controller.GetName() + "'",
                         MESSAGE_MS);
    return;
  }

  Core::DisplayMessage("Loading input profile '" + profile_name + "' for device '" +
                           controller.GetName() + "'",
                       MESSAGE_MS);
  controller.LoadConfig(ini.GetOrCreateSection("Profile"));
  controller.UpdateReferences(g_controller_interface);
  config.GenerateControllerTextures(ini);
}

std::string GameProfileSetting(int controller_index)
{
  Common::IniFile game_ini = SConfig::GetInstance().LoadGameIni();
  const Common::IniFile::Section* const controls = game_ini.GetOrCreateSection("Controls");

  std::string setting;
  controls->Get(fmt::format("WiimoteProfile{}", controller_index + 1), &setting);
  return setting;
}
}  // namespace

std::vector<std::string> GetProfilesFromSetting(const std::string& setting, const std::string& root)
{
  std::vector<std::string> profiles;
  for (const std::string& entry : SplitString(setting, ','))
  {
    const std::string path = root + std::string(StripWhitespace(entry));

    // A directory contributes every profile below it.
    if (File::IsDirectory(path))
    {
      std::vector<std::string> inside = Common::DoFileSearch(path, ".ini", true);
      profiles.insert(profiles.end(), inside.begin(), inside.end());
      continue;
    }

    std::string file = path + ".ini";
    if (File::Exists(file))
      profiles.push_back(std::move(file));
  }
  return profiles;
}

void ProfileCycler::Cycle(CycleDirection direction, int controller_index,
                          const std::string* game_setting)
{
  InputConfig* const config = Wiimote::GetConfig();

  std::vector<std::string> candidates = FindAllProfiles(*config);
  if (candidates.empty())
  {
    Core::DisplayMessage("No input profiles found", MESSAGE_MS);
    return;
  }

  if (game_setting != nullptr)
  {
    if (game_setting->empty())
    {
      Core::DisplayMessage("No setting found for game", MESSAGE_MS);
      return;
    }

    // Only the user's profiles the game names (both lists are sorted).
    const std::vector<std::string> named =
        GetProfilesFromSetting(*game_setting, config->GetUserProfileDirectoryPath());
    std::vector<std::string> for_game;
    if (!named.empty())
      std::ranges::set_intersection(candidates, named, std::back_inserter(for_game));

    if (for_game.empty())
    {
      Core::DisplayMessage("No input profiles found for game", MESSAGE_MS);
      return;
    }
    candidates = std::move(for_game);
  }

  const std::string& profile = StepThrough(candidates, m_wiimote_profile_index, direction);

  ControllerEmu::EmulatedController* const controller = config->GetController(controller_index);
  if (controller == nullptr)
  {
    Core::DisplayMessage(fmt::format("No controller found for index: {}", controller_index),
                         MESSAGE_MS);
    return;
  }
  LoadProfileInto(profile, *controller, *config);
}

void ProfileCycler::NextWiimoteProfile(int controller_index)
{
  Cycle(CycleDirection::Forward, controller_index, nullptr);
}

void ProfileCycler::PreviousWiimoteProfile(int controller_index)
{
  Cycle(CycleDirection::Backward, controller_index, nullptr);
}

void ProfileCycler::NextWiimoteProfileForGame(int controller_index)
{
  const std::string setting = GameProfileSetting(controller_index);
  Cycle(CycleDirection::Forward, controller_index, &setting);
}

void ProfileCycler::PreviousWiimoteProfileForGame(int controller_index)
{
  const std::string setting = GameProfileSetting(controller_index);
  Cycle(CycleDirection::Backward, controller_index, &setting);
}
}  // namespace InputProfile
