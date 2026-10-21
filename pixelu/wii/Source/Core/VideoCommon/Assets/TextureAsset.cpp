// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Assets/TextureAsset.h"

#include <array>
#include <span>
#include <string_view>
#include <utility>

#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "VideoCommon/BPMemory.h"

namespace VideoCommon
{
namespace
{
template <typename Mode>
using ModeName = std::pair<std::string_view, Mode>;

constexpr std::array<ModeName<WrapMode>, 3> WRAP_NAMES{{
    {"clamp", WrapMode::Clamp},
    {"repeat", WrapMode::Repeat},
    {"mirror", WrapMode::Mirror},
}};

constexpr std::array<ModeName<FilterMode>, 2> FILTER_NAMES{{
    {"linear", FilterMode::Linear},
    {"near", FilterMode::Near},
}};

constexpr std::array<std::pair<std::string_view, AbstractTextureType>, 3> TYPE_NAMES{{
    {"texture2d", AbstractTextureType::Texture_2D},
    {"texturecube", AbstractTextureType::Texture_CubeMap},
    {"texture2darray", AbstractTextureType::Texture_2DArray},
}};

template <typename Mode>
std::string_view NameOf(std::span<const ModeName<Mode>> names, Mode mode)
{
  for (const auto& [name, value] : names)
  {
    if (value == mode)
      return name;
  }
  return {};
}

// Reads group[key] as one of `names` (case-insensitive) into *out.
template <typename Mode>
bool ReadMode(const CustomAssetLibrary::AssetID& asset_id, const picojson::object& group,
              std::string_view group_name, const std::string& key,
              std::span<const ModeName<Mode>> names, Mode* out)
{
  std::string text = ReadStringFromJson(group, key).value_or("");
  Common::ToLower(&text);
  for (const auto& [name, value] : names)
  {
    if (text == name)
    {
      *out = value;
      return true;
    }
  }

  ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, '{}[{}]' has an invalid value", asset_id,
                group_name, key);
  return false;
}

// Returns the object stored under `name`, null if absent, or reports a type error.
bool FindGroup(const CustomAssetLibrary::AssetID& asset_id, const picojson::object& json,
               const std::string& name, const picojson::object** group)
{
  *group = nullptr;
  const auto it = json.find(name);
  if (it == json.end())
    return true;
  if (!it->second.is<picojson::object>())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, '{}' is not the right type", asset_id,
                  name);
    return false;
  }
  *group = &it->second.get<picojson::object>();
  return true;
}

// Starts from the linear sampler and applies the optional "wrap_mode" and "filter_mode" groups.
// Inside a group that is present, every field is required.
bool ReadSampler(const CustomAssetLibrary::AssetID& asset_id, const picojson::object& json,
                 SamplerState* sampler)
{
  *sampler = RenderState::GetLinearSamplerState();
  auto& tm0 = sampler->tm0;

  const picojson::object* wrap = nullptr;
  if (!FindGroup(asset_id, json, "wrap_mode", &wrap))
    return false;
  if (wrap != nullptr)
  {
    const std::span<const ModeName<WrapMode>> names = WRAP_NAMES;
    WrapMode mode{};
    if (!ReadMode(asset_id, *wrap, "wrap_mode", "u", names, &mode))
      return false;
    tm0.wrap_u = mode;
    if (!ReadMode(asset_id, *wrap, "wrap_mode", "v", names, &mode))
      return false;
    tm0.wrap_v = mode;
  }

  const picojson::object* filter = nullptr;
  if (!FindGroup(asset_id, json, "filter_mode", &filter))
    return false;
  if (filter != nullptr)
  {
    const std::span<const ModeName<FilterMode>> names = FILTER_NAMES;
    FilterMode mode{};
    if (!ReadMode(asset_id, *filter, "filter_mode", "min", names, &mode))
      return false;
    tm0.min_filter = mode;
    if (!ReadMode(asset_id, *filter, "filter_mode", "mag", names, &mode))
      return false;
    tm0.mag_filter = mode;
    if (!ReadMode(asset_id, *filter, "filter_mode", "mipmap", names, &mode))
      return false;
    tm0.mipmap_filter = mode;
  }

  return true;
}
}  // namespace

bool TextureAndSamplerData::FromJson(const CustomAssetLibrary::AssetID& asset_id,
                                     const picojson::object& json, TextureAndSamplerData* data)
{
  const auto entry = json.find("type");
  if (entry == json.end())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, property entry 'type' not found",
                  asset_id);
    return false;
  }
  if (!entry->second.is<std::string>())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Asset '{}' failed to parse json, property entry 'type' is not the right json "
                  "type",
                  asset_id);
    return false;
  }

  std::string type = entry->second.to_str();
  Common::ToLower(&type);
  const auto known = std::ranges::find(TYPE_NAMES, std::string_view{type},
                                       &std::pair<std::string_view, AbstractTextureType>::first);
  if (known == TYPE_NAMES.end())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, texture type '{}' an invalid option",
                  asset_id, type);
    return false;
  }

  data->type = known->second;
  // Only 2D textures carry their own sampler settings.
  if (data->type == AbstractTextureType::Texture_2D)
    return ReadSampler(asset_id, json, &data->sampler);
  return true;
}

void TextureAndSamplerData::ToJson(picojson::object* obj, const TextureAndSamplerData& data)
{
  if (obj == nullptr) [[unlikely]]
    return;

  for (const auto& [name, type] : TYPE_NAMES)
  {
    if (type == data.type)
    {
      obj->emplace("type", std::string{name});
      break;
    }
  }

  const auto& tm0 = data.sampler.tm0;
  const std::span<const ModeName<WrapMode>> wraps = WRAP_NAMES;
  const std::span<const ModeName<FilterMode>> filters = FILTER_NAMES;

  picojson::object wrap;
  wrap.emplace("u", std::string{NameOf(wraps, tm0.wrap_u.Value())});
  wrap.emplace("v", std::string{NameOf(wraps, tm0.wrap_v.Value())});
  obj->emplace("wrap_mode", std::move(wrap));

  picojson::object filter;
  filter.emplace("min", std::string{NameOf(filters, tm0.min_filter.Value())});
  filter.emplace("mag", std::string{NameOf(filters, tm0.mag_filter.Value())});
  filter.emplace("mipmap", std::string{NameOf(filters, tm0.mipmap_filter.Value())});
  obj->emplace("filter_mode", std::move(filter));
}

CustomAssetLibrary::LoadInfo TextureAsset::LoadImpl(const CustomAssetLibrary::AssetID& asset_id)
{
  return ReadAndPublish(
      [&](CustomTextureData* data) { return m_owning_library->LoadTexture(asset_id, data); });
}

CustomAssetLibrary::LoadInfo
TextureAndSamplerAsset::LoadImpl(const CustomAssetLibrary::AssetID& asset_id)
{
  return ReadAndPublish(
      [&](TextureAndSamplerData* data) { return m_owning_library->LoadTexture(asset_id, data); });
}
}  // namespace VideoCommon
