// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Config/GraphicsModFeature.h"

#include "Common/Logging/Log.h"

void GraphicsModFeatureConfig::SerializeToConfig(picojson::object& json_obj) const
{
  json_obj.emplace("group", m_group);
  json_obj.emplace("action", m_action);
  json_obj.emplace("action_data", m_action_data);
}

bool GraphicsModFeatureConfig::DeserializeFromConfig(const picojson::object& obj)
{
  // All three members are optional; present ones must have the right type.
  const auto group = obj.find("group");
  if (group != obj.end())
  {
    if (!group->second.is<std::string>())
    {
      ERROR_LOG_FMT(
          VIDEO,
          "Failed to load mod configuration file, specified feature's group is not a string");
      return false;
    }
    m_group = group->second.get<std::string>();
  }

  const auto action = obj.find("action");
  if (action != obj.end())
  {
    if (!action->second.is<std::string>())
    {
      ERROR_LOG_FMT(
          VIDEO,
          "Failed to load mod configuration file, specified feature's action is not a string");
      return false;
    }
    m_action = action->second.get<std::string>();
  }

  // The action interprets its own data later, so any JSON value is accepted here.
  const auto action_data = obj.find("action_data");
  if (action_data != obj.end())
    m_action_data = action_data->second;

  return true;
}

void GraphicsModFeatureConfig::SerializeToProfile(picojson::object*) const
{
}

void GraphicsModFeatureConfig::DeserializeFromProfile(const picojson::object&)
{
}
