// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#include "VideoCommon/GraphicsModSystem/Constants.h"

namespace
{
std::string ModDirectory(GraphicsModConfig::Source source)
{
  if (source == GraphicsModConfig::Source::User)
    return File::GetUserPath(D_GRAPHICSMOD_IDX);
  return File::GetSysDirectory() + DOLPHIN_SYSTEM_GRAPHICS_MOD_DIR;
}

// Reads the optional array value[key]; each element must be an object that `Part` accepts.
template <typename Part>
bool ReadParts(const picojson::value& value, const std::string& key, std::string_view what,
               std::vector<Part>* parts)
{
  const picojson::value& list = value.get(key);
  if (!list.is<picojson::array>())
    return true;

  for (const picojson::value& entry : list.get<picojson::array>())
  {
    if (!entry.is<picojson::object>())
    {
      ERROR_LOG_FMT(
          VIDEO, "Failed to load mod configuration file, specified {} is not a json object", what);
      return false;
    }
    Part part;
    if (!part.DeserializeFromConfig(entry.get<picojson::object>()))
      return false;
    parts->push_back(std::move(part));
  }
  return true;
}

template <typename Part, typename Writer>
picojson::array PartsToJson(const std::vector<Part>& parts, Writer write)
{
  picojson::array out;
  for (const Part& part : parts)
  {
    picojson::object entry;
    write(part, entry);
    out.emplace_back(std::move(entry));
  }
  return out;
}

// Applies saved per-part profile data. Returns false when the saved list does not line up
// with the mod (it was saved for a different version), which ends profile loading.
template <typename Part>
bool ApplyPartProfiles(const picojson::object& obj, const std::string& key,
                       std::vector<Part>* parts)
{
  const auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<picojson::array>())
    return true;

  const picojson::array& saved = it->second.get<picojson::array>();
  if (saved.size() != parts->size())
    return false;

  for (std::size_t i = 0; i < saved.size(); ++i)
  {
    if (saved[i].is<picojson::object>())
      (*parts)[i].DeserializeFromProfile(saved[i].get<picojson::object>());
  }
  return true;
}

void CopyStringIfPresent(const picojson::value& object, const std::string& key, std::string* out)
{
  const picojson::value& member = object.get(key);
  if (member.is<std::string>())
    *out = member.get<std::string>();
}
}  // namespace

std::optional<GraphicsModConfig> GraphicsModConfig::Create(const std::string& file_path,
                                                           Source source)
{
  std::string parse_error;
  picojson::value root;
  if (!JsonFromFile(file_path, &root, &parse_error))
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load graphics mod json file '{}' due to parse error: {}",
                  file_path, parse_error);
    return std::nullopt;
  }

  GraphicsModConfig mod;
  if (!mod.DeserializeFromConfig(root))
    return std::nullopt;
  mod.m_source = source;

  // The relative path is whatever follows the directory's length; only the length is checked.
  const std::string base = ModDirectory(source);
  if (base.size() > file_path.size())
  {
    ERROR_LOG_FMT(
        VIDEO,
        "Failed to load graphics mod json file '{}' due to it not matching the base path: {}",
        file_path, base);
    return std::nullopt;
  }
  mod.m_relative_path = file_path.substr(base.size());
  return mod;
}

std::optional<GraphicsModConfig> GraphicsModConfig::Create(const picojson::object* obj)
{
  if (obj == nullptr)
    return std::nullopt;

  const auto source = obj->find("source");
  if (source == obj->end())
    return std::nullopt;
  const auto path = obj->find("path");
  if (path == obj->end())
    return std::nullopt;

  const Source kind = source->second.to_str() == "system" ? Source::System : Source::User;
  return Create(ModDirectory(kind) + path->second.to_str(), kind);
}

std::string GraphicsModConfig::GetAbsolutePath() const
{
  return WithUnifiedPathSeparators(ModDirectory(m_source) + m_relative_path);
}

void GraphicsModConfig::SerializeToConfig(picojson::object& json_obj) const
{
  picojson::object meta;
  meta.emplace("title", m_title);
  meta.emplace("author", m_author);
  meta.emplace("description", m_description);
  json_obj.emplace("meta", std::move(meta));

  json_obj.emplace("groups", PartsToJson(m_groups, [](const auto& p, picojson::object& o) {
                     p.SerializeToConfig(o);
                   }));
  json_obj.emplace("features", PartsToJson(m_features, [](const auto& p, picojson::object& o) {
                     p.SerializeToConfig(o);
                   }));
  json_obj.emplace("assets", PartsToJson(m_assets, [](const auto& p, picojson::object& o) {
                     p.SerializeToConfig(o);
                   }));
}

bool GraphicsModConfig::DeserializeFromConfig(const picojson::value& value)
{
  // Metadata is optional and malformed members are ignored.
  const picojson::value& meta = value.get("meta");
  if (meta.is<picojson::object>())
  {
    CopyStringIfPresent(meta, "title", &m_title);
    CopyStringIfPresent(meta, "author", &m_author);
    CopyStringIfPresent(meta, "description", &m_description);
  }

  return ReadParts(value, "groups", "group", &m_groups) &&
         ReadParts(value, "features", "feature", &m_features) &&
         ReadParts(value, "assets", "asset", &m_assets);
}

void GraphicsModConfig::SerializeToProfile(picojson::object* obj) const
{
  if (obj == nullptr)
    return;

  obj->emplace("source", m_source == Source::System ? "system" : "user");
  obj->emplace("path", m_relative_path);
  obj->emplace("groups", PartsToJson(m_groups, [](const auto& p, picojson::object& o) {
                 p.SerializeToProfile(&o);
               }));
  obj->emplace("features", PartsToJson(m_features, [](const auto& p, picojson::object& o) {
                 p.SerializeToProfile(&o);
               }));
  obj->emplace("enabled", m_enabled);
  obj->emplace("weight", static_cast<double>(m_weight));
}

void GraphicsModConfig::DeserializeFromProfile(const picojson::object& obj)
{
  if (!ApplyPartProfiles(obj, "groups", &m_groups))
    return;
  if (!ApplyPartProfiles(obj, "features", &m_features))
    return;

  if (const auto enabled = obj.find("enabled"); enabled != obj.end() && enabled->second.is<bool>())
  {
    m_enabled = enabled->second.get<bool>();
  }
  if (const auto weight = obj.find("weight"); weight != obj.end() && weight->second.is<double>())
    m_weight = static_cast<u16>(weight->second.get<double>());
}
