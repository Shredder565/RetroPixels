// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace DiscIO
{
// A game mod descriptor is a small JSON file ("type": "dolphin-game-mod-descriptor",
// "version": 1) that makes a modded game show up in the game list: it names the base image and
// the Riivolution patches to apply, plus an optional title, maker and banner. Relative paths in
// the file are relative to the file's own folder.

// A preselected choice for one Riivolution option, matched by section name and option ID or name.
struct GameModDescriptorRiivolutionPatchOption
{
  std::string section_name;
  std::string option_id;
  std::string option_name;
  u32 choice = 0;
};

struct GameModDescriptorRiivolutionPatch
{
  std::string xml;
  std::string root;
  std::vector<GameModDescriptorRiivolutionPatchOption> options;
};

struct GameModDescriptorRiivolution
{
  std::vector<GameModDescriptorRiivolutionPatch> patches;
};

struct GameModDescriptor
{
  std::string base_file;
  std::string display_name;
  std::string maker;
  std::string banner;
  std::optional<GameModDescriptorRiivolution> riivolution = std::nullopt;
};

std::optional<GameModDescriptor> ParseGameModDescriptorFile(const std::string& filename);
std::optional<GameModDescriptor> ParseGameModDescriptorString(std::string_view json,
                                                              std::string_view json_path);
std::string WriteGameModDescriptorString(const GameModDescriptor& descriptor, bool pretty);
bool WriteGameModDescriptorFile(const std::string& filename, const GameModDescriptor& descriptor,
                                bool pretty);
}  // namespace DiscIO
