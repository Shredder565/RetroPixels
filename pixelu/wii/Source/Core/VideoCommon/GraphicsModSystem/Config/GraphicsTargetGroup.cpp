// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Config/GraphicsTargetGroup.h"

#include "Common/Logging/Log.h"

namespace
{
// Serializes every target with `write` into a JSON array.
template <typename Writer>
picojson::array TargetsToJson(const std::vector<GraphicsTargetConfig>& targets, Writer write)
{
  picojson::array out;
  out.reserve(targets.size());
  for (const GraphicsTargetConfig& target : targets)
  {
    picojson::object entry;
    write(entry, target);
    out.emplace_back(std::move(entry));
  }
  return out;
}
}  // namespace

void GraphicsTargetGroupConfig::SerializeToConfig(picojson::object& json_obj) const
{
  json_obj.emplace("targets",
                   TargetsToJson(m_targets, [](picojson::object& entry, const auto& target) {
                     SerializeTargetToConfig(entry, target);
                   }));
  json_obj.emplace("name", m_name);
}

bool GraphicsTargetGroupConfig::DeserializeFromConfig(const picojson::object& obj)
{
  if (const auto name = obj.find("name"); name != obj.end())
  {
    if (!name->second.is<std::string>())
    {
      ERROR_LOG_FMT(
          VIDEO, "Failed to load mod configuration file, specified group's name is not a string");
      return false;
    }
    m_name = name->second.get<std::string>();
  }

  const auto targets = obj.find("targets");
  if (targets == obj.end())
    return true;

  if (!targets->second.is<picojson::array>())
  {
    ERROR_LOG_FMT(
        VIDEO, "Failed to load mod configuration file, specified group's targets is not an array");
    return false;
  }

  for (const picojson::value& entry : targets->second.get<picojson::array>())
  {
    if (!entry.is<picojson::object>())
    {
      ERROR_LOG_FMT(
          VIDEO, "Failed to load shader configuration file, specified target is not a json object");
      return false;
    }
    std::optional<GraphicsTargetConfig> target =
        DeserializeTargetFromConfig(entry.get<picojson::object>());
    if (!target)
      return false;
    m_targets.push_back(std::move(*target));
  }
  return true;
}

void GraphicsTargetGroupConfig::SerializeToProfile(picojson::object* obj) const
{
  if (obj == nullptr)
    return;
  obj->emplace("targets", TargetsToJson(m_targets, [](picojson::object& entry, const auto& target) {
                 SerializeTargetToProfile(&entry, target);
               }));
}

void GraphicsTargetGroupConfig::DeserializeFromProfile(const picojson::object& obj)
{
  const auto targets = obj.find("targets");
  if (targets == obj.end() || !targets->second.is<picojson::array>())
    return;

  // A profile saved for a different version of the group does not apply.
  const picojson::array& saved = targets->second.get<picojson::array>();
  if (saved.size() != m_targets.size())
    return;

  for (std::size_t i = 0; i < saved.size(); ++i)
  {
    if (saved[i].is<picojson::object>())
      DeserializeTargetFromProfile(saved[i].get<picojson::object>(), &m_targets[i]);
  }
}
