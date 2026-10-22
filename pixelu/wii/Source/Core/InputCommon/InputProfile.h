// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

class InputConfig;

namespace ControllerEmu
{
class EmulatedController;
}

#include <string>
#include <vector>

namespace InputProfile
{
// Profile files named by a comma-separated setting (as in a game ini): each entry is a profile
// name or a directory of profiles, relative to `root`.
std::vector<std::string> GetProfilesFromSetting(const std::string& setting,
                                                const std::string& root);

enum class CycleDirection : int
{
  Forward = 1,
  Backward = -1
};

// Steps a Wii Remote through the user's input profiles (hotkeys "next/previous profile").
class ProfileCycler
{
public:
  void NextWiimoteProfile(int controller_index);
  void PreviousWiimoteProfile(int controller_index);
  // Same, limited to the profiles the running game's ini names.
  void NextWiimoteProfileForGame(int controller_index);
  void PreviousWiimoteProfileForGame(int controller_index);

private:
  // `game_setting` is null to cycle through all profiles.
  void Cycle(CycleDirection direction, int controller_index, const std::string* game_setting);

  int m_wiimote_profile_index = 0;
};
}  // namespace InputProfile
