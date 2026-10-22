// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/DynamicInputTextureManager.h"

#include <set>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Core/ConfigManager.h"

#include "InputCommon/DynamicInputTextures/DITConfiguration.h"
#include "InputCommon/ImageOperations.h"
#include "VideoCommon/HiresTextures.h"

namespace InputCommon
{
DynamicInputTextureManager::DynamicInputTextureManager() = default;

DynamicInputTextureManager::~DynamicInputTextureManager() = default;

void DynamicInputTextureManager::Load()
{
  m_configuration.clear();

  // The pack directories of the most specific game id that has any.
  const std::set<std::string> directories =
      GetTextureDirectoriesForFirstMatchingGameId(File::GetUserPath(D_DYNAMICINPUT_IDX),
                                                  SConfig::GetInstance().GetGameIDsForTextures());

  for (const std::string& directory : directories)
  {
    for (const std::string& json_path : Common::DoFileSearch(directory, ".json"))
      m_configuration.emplace_back(json_path);
  }
}

void DynamicInputTextureManager::GenerateTextures(const Common::IniFile& file,
                                                  const std::vector<std::string>& controller_names)
{
  DynamicInputTextures::OutputDetails generated;
  for (const DynamicInputTextures::Configuration& configuration : m_configuration)
    configuration.GenerateTextures(file, controller_names, &generated);

  // Each output folder is marked for the game's preferred texture id with an empty
  // "gameids/<id>.txt" file.
  const std::string& game_id = SConfig::GetInstance().GetGameIDForTextures();
  for (const auto& [folder_name, images] : generated)
  {
    const std::string folder = File::GetUserPath(D_HIRESTEXTURES_IDX) + folder_name;
    if (!File::IsDirectory(folder))
      File::CreateDir(folder);

    const std::string id_folder = folder + DIR_SEP + "gameids";
    if (!File::IsDirectory(id_folder))
      File::CreateDir(id_folder);
    File::CreateEmptyFile(id_folder + DIR_SEP + game_id + ".txt");

    for (const auto& [image_name, image] : images)
      WriteImage(folder + DIR_SEP + image_name, image);
  }
}
}  // namespace InputCommon
