// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/IniFile.h"

#include <string>
#include <vector>

namespace InputCommon
{
namespace DynamicInputTextures
{
class Configuration;
}

// Dynamic input textures: texture packs that draw the user's own key/button images into a
// game's button prompts. The packs live in <User>/Load/DynamicInputTextures/<game id>/ as JSON
// files; the generated textures are written as hi-res textures.
class DynamicInputTextureManager
{
public:
  DynamicInputTextureManager();
  ~DynamicInputTextureManager();

  // Reads the pack descriptions for the running game.
  void Load();
  // Draws the textures for the mappings in `file` (one section per controller name).
  void GenerateTextures(const Common::IniFile& file,
                        const std::vector<std::string>& controller_names);

private:
  std::vector<DynamicInputTextures::Configuration> m_configuration;
  std::string m_config_type;
};
}  // namespace InputCommon
