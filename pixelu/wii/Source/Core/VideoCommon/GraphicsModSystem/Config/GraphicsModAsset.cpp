// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Config/GraphicsModAsset.h"

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

void GraphicsModAssetConfig::SerializeToConfig(picojson::object& json_obj) const
{
  json_obj.emplace("name", m_asset_id);

  picojson::object files;
  for (const auto& [role, path] : m_map)
    files.emplace(role, PathToString(path));
  json_obj.emplace("data", std::move(files));
}

bool GraphicsModAssetConfig::DeserializeFromConfig(const picojson::object& obj)
{
  const auto name = obj.find("name");
  if (name == obj.end())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, specified asset has no name");
    return false;
  }
  if (!name->second.is<std::string>())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, specified asset has a name that "
                         "is not a string");
    return false;
  }
  m_asset_id = name->second.get<std::string>();

  const auto files = obj.find("data");
  if (files == obj.end())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, specified asset '{}' has no data",
                  m_asset_id);
    return false;
  }
  if (!files->second.is<picojson::object>())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load mod configuration file, specified asset '{}' has data that is "
                  "not an object",
                  m_asset_id);
    return false;
  }

  for (const auto& [role, file] : files->second.get<picojson::object>())
  {
    if (!file.is<std::string>())
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load mod configuration file, specified asset '{}' has data with a "
                    "value for key '{}' that is not a string",
                    m_asset_id, role);
      return false;
    }
    m_map[role] = file.get<std::string>();
  }
  return true;
}
