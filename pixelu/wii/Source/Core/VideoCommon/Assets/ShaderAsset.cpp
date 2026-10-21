// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Assets/ShaderAsset.h"

#include <algorithm>
#include <type_traits>
#include <utility>

#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "VideoCommon/Assets/CustomAssetLibrary.h"
#include "VideoCommon/ShaderGenCommon.h"

namespace VideoCommon
{
namespace
{
using Value = ShaderProperty::Value;

// One property type: its JSON name, how it is declared in shader code, and which alternative
// of ShaderProperty::Value holds it.
struct PropertyType
{
  std::string_view name;
  std::string_view shader_type;
  u32 components;
  std::size_t alternative;
};

// In the order GetValueTypeNames reports them.
constexpr std::array<PropertyType, 11> PROPERTY_TYPES{{
    {"int", "int", 1, 0},
    {"int2", "int", 2, 1},
    {"int3", "int", 3, 2},
    {"int4", "int", 4, 3},
    {"float", "float", 1, 4},
    {"float2", "float", 2, 5},
    {"float3", "float", 3, 6},
    {"float4", "float", 4, 7},
    {"rgb", "float", 3, 9},
    {"rgba", "float", 4, 10},
    {"bool", "bool", 1, 8},
}};
static_assert(std::variant_size_v<Value> == PROPERTY_TYPES.size());

constexpr auto PROPERTY_TYPE_NAMES = [] {
  std::array<std::string_view, PROPERTY_TYPES.size()> names{};
  for (std::size_t i = 0; i < names.size(); ++i)
    names[i] = PROPERTY_TYPES[i].name;
  return names;
}();

const PropertyType* FindTypeByName(std::string_view name)
{
  const auto it = std::ranges::find(PROPERTY_TYPES, name, &PropertyType::name);
  return it != PROPERTY_TYPES.end() ? &*it : nullptr;
}

const PropertyType& TypeOfValue(const Value& value)
{
  return *std::ranges::find(PROPERTY_TYPES, value.index(), &PropertyType::alternative);
}

template <std::size_t... I>
Value ZeroOfAlternative(std::size_t alternative, std::index_sequence<I...>)
{
  Value result;
  (void)((alternative == I ? (result.template emplace<I>(), true) : false) || ...);
  return result;
}

// Reads `count` JSON numbers (a bare number when count is 1) converted to Element.
template <typename Element, std::size_t N>
bool ReadNumbers(const CustomAssetLibrary::AssetID& asset_id, const picojson::value& json,
                 std::string_view code_name, std::array<Element, N>* out)
{
  if constexpr (N == 1)
  {
    if (!json.is<double>())
    {
      ERROR_LOG_FMT(VIDEO,
                    "Asset id '{}' shader has attribute '{}' where a double was expected but not "
                    "provided.",
                    asset_id, code_name);
      return false;
    }
    (*out)[0] = static_cast<Element>(json.get<double>());
    return true;
  }
  else
  {
    if (!json.is<picojson::array>())
    {
      ERROR_LOG_FMT(VIDEO,
                    "Asset id '{}' shader has attribute '{}' where an array was expected but not "
                    "provided.",
                    asset_id, code_name);
      return false;
    }

    const picojson::array& items = json.get<picojson::array>();
    if (items.size() != N)
    {
      ERROR_LOG_FMT(VIDEO,
                    "Asset id '{}' shader has attribute '{}' with incorrect number of elements, "
                    "expected {}",
                    asset_id, code_name, N);
      return false;
    }
    if (!std::ranges::all_of(items, [](const picojson::value& v) { return v.is<double>(); }))
    {
      ERROR_LOG_FMT(VIDEO,
                    "Asset id '{}' shader has attribute '{}' where all elements are not of type "
                    "double.",
                    asset_id, code_name);
      return false;
    }

    for (std::size_t i = 0; i < N; ++i)
      (*out)[i] = static_cast<Element>(items[i].get<double>());
    return true;
  }
}

void ReportBadValue(const CustomAssetLibrary::AssetID& asset_id, std::string_view type)
{
  ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse the json, value is not valid for type '{}'",
                asset_id, type);
}

// Parses `json` as alternative I of Value.
template <std::size_t I>
bool ReadAlternative(const CustomAssetLibrary::AssetID& asset_id, const picojson::value& json,
                     std::string_view code_name, std::string_view type, Value* out)
{
  using Alt = std::variant_alternative_t<I, Value>;
  if constexpr (std::is_same_v<Alt, bool>)
  {
    if (!json.is<bool>())
    {
      ReportBadValue(asset_id, type);
      return false;
    }
    *out = json.get<bool>();
    return true;
  }
  else if constexpr (std::is_same_v<Alt, ShaderProperty::RGB> ||
                     std::is_same_v<Alt, ShaderProperty::RGBA>)
  {
    Alt colour;
    if (!ReadNumbers(asset_id, json, code_name, &colour.value))
      return false;
    *out = colour;
    return true;
  }
  else if constexpr (std::is_arithmetic_v<Alt>)
  {
    std::array<Alt, 1> scalar;
    if (!ReadNumbers(asset_id, json, code_name, &scalar))
      return false;
    *out = scalar[0];
    return true;
  }
  else
  {
    Alt numbers;
    if (!ReadNumbers(asset_id, json, code_name, &numbers))
      return false;
    *out = numbers;
    return true;
  }
}

template <std::size_t... I>
bool ReadValueOfType(const CustomAssetLibrary::AssetID& asset_id, const picojson::value& json,
                     std::string_view code_name, const PropertyType& type, Value* out,
                     std::index_sequence<I...>)
{
  bool ok = false;
  (void)((type.alternative == I ?
              (ok = ReadAlternative<I>(asset_id, json, code_name, type.name, out), true) :
              false) ||
         ...);
  return ok;
}

bool ReadValue(const CustomAssetLibrary::AssetID& asset_id, const picojson::value& json,
               std::string_view code_name, std::string_view type_name, Value* out)
{
  const PropertyType* type = FindTypeByName(type_name);
  if (type == nullptr)
  {
    ReportBadValue(asset_id, type_name);
    return false;
  }
  return ReadValueOfType(asset_id, json, code_name, *type, out,
                         std::make_index_sequence<std::variant_size_v<Value>>{});
}

// Looks up a required string member of a property object.
const std::string* RequiredString(const CustomAssetLibrary::AssetID& asset_id,
                                  const picojson::object& obj, const std::string& key)
{
  const auto it = obj.find(key);
  if (it == obj.end())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, property entry '{}' not found", asset_id,
                  key);
    return nullptr;
  }
  if (!it->second.is<std::string>())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Asset '{}' failed to parse json, property entry '{}' is not the right json type",
                  asset_id, key);
    return nullptr;
  }
  return &it->second.get<std::string>();
}

bool ReadProperty(const CustomAssetLibrary::AssetID& asset_id, const picojson::value& json,
                  ShaderProperty* property)
{
  if (!json.is<picojson::object>())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, property is not the right json type",
                  asset_id);
    return false;
  }
  const picojson::object& obj = json.get<picojson::object>();

  const std::string* type_text = RequiredString(asset_id, obj, "type");
  if (type_text == nullptr)
    return false;
  std::string type = *type_text;
  Common::ToLower(&type);

  const std::string* description = RequiredString(asset_id, obj, "description");
  if (description == nullptr)
    return false;
  property->description = *description;

  const std::string* code_name = RequiredString(asset_id, obj, "code_name");
  if (code_name == nullptr)
    return false;
  property->name = *code_name;

  const auto default_entry = obj.find("default");
  if (default_entry == obj.end())
  {
    property->default_value = ShaderProperty::GetDefaultValueFromTypeName(type);
    return true;
  }
  return ReadValue(asset_id, default_entry->second, property->name, type, &property->default_value);
}

// Finds json[name] and checks that it is an array.
const picojson::array* RequiredArray(const CustomAssetLibrary::AssetID& asset_id,
                                     const picojson::object& json, const std::string& name)
{
  const auto it = json.find(name);
  if (it == json.end())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, '{}' not found", asset_id, name);
    return nullptr;
  }
  if (!it->second.is<picojson::array>())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, '{}' is not the right json type",
                  asset_id, name);
    return nullptr;
  }
  return &it->second.get<picojson::array>();
}

bool ReadSamplers(const CustomAssetLibrary::AssetID& asset_id, const picojson::array& entries,
                  std::vector<RasterSurfaceShaderData::SamplerData>* samplers)
{
  if (!std::ranges::all_of(entries,
                           [](const picojson::value& v) { return v.is<picojson::object>(); }))
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, '{}' must contain objects", asset_id,
                  "samplers");
    return false;
  }

  for (const picojson::value& entry : entries)
  {
    const picojson::object& obj = entry.get<picojson::object>();
    RasterSurfaceShaderData::SamplerData sampler;

    const std::optional<std::string> name = ReadStringFromJson(obj, "name");
    if (!name)
    {
      ERROR_LOG_FMT(VIDEO,
                    "Asset '{}' failed to parse sampler json, 'name' not found or wrong type",
                    asset_id);
      return false;
    }
    sampler.name = *name;

    const std::optional<AbstractTextureType> type =
        ReadNumericFromJson<AbstractTextureType>(obj, "type");
    if (!type)
    {
      ERROR_LOG_FMT(VIDEO,
                    "Asset '{}' failed to parse sampler json, 'type' not found or wrong type",
                    asset_id);
      return false;
    }
    sampler.type = *type;

    samplers->push_back(std::move(sampler));
  }
  return true;
}

template <typename T>
picojson::value DefaultToJson(const T& value)
{
  if constexpr (std::is_same_v<T, bool>)
    return picojson::value(value);
  else if constexpr (std::is_arithmetic_v<T>)
    return picojson::value(static_cast<double>(value));
  else if constexpr (std::is_same_v<T, ShaderProperty::RGB> ||
                     std::is_same_v<T, ShaderProperty::RGBA>)
    return picojson::value(ToJsonArray(value.value));
  else
    return picojson::value(ToJsonArray(value));
}
}  // namespace

bool RasterSurfaceShaderData::FromJson(const VideoCommon::CustomAssetLibrary::AssetID& asset_id,
                                       const picojson::object& json, RasterSurfaceShaderData* data)
{
  const picojson::array* properties = RequiredArray(asset_id, json, "properties");
  if (properties == nullptr)
    return false;
  for (const picojson::value& entry : *properties)
  {
    ShaderProperty property;
    if (!ReadProperty(asset_id, entry, &property))
      return false;
    data->uniform_properties.push_back(std::move(property));
  }

  const picojson::array* samplers = RequiredArray(asset_id, json, "samplers");
  if (samplers == nullptr)
    return false;
  return ReadSamplers(asset_id, *samplers, &data->samplers);
}

void RasterSurfaceShaderData::ToJson(picojson::object& obj, const RasterSurfaceShaderData& data)
{
  picojson::array properties;
  for (const ShaderProperty& property : data.uniform_properties)
  {
    picojson::object entry;
    entry.emplace("code_name", property.name);
    entry.emplace("description", property.description);
    entry.emplace("type", std::string{TypeOfValue(property.default_value).name});
    entry.emplace("default", std::visit([](const auto& v) { return DefaultToJson(v); },
                                        property.default_value));
    properties.emplace_back(std::move(entry));
  }
  obj.emplace("properties", std::move(properties));

  picojson::array samplers;
  for (const SamplerData& sampler : data.samplers)
  {
    picojson::object entry;
    entry.emplace("name", sampler.name);
    entry.emplace("type", static_cast<double>(sampler.type));
    samplers.emplace_back(std::move(entry));
  }
  obj.emplace("samplers", std::move(samplers));
}

std::span<const std::string_view> ShaderProperty::GetValueTypeNames()
{
  return PROPERTY_TYPE_NAMES;
}

ShaderProperty::Value ShaderProperty::GetDefaultValueFromTypeName(std::string_view name)
{
  const PropertyType* type = FindTypeByName(name);
  if (type == nullptr)
    return Value{};
  return ZeroOfAlternative(type->alternative,
                           std::make_index_sequence<std::variant_size_v<Value>>{});
}

void ShaderProperty::WriteAsShaderCode(ShaderCode& shader_source, const ShaderProperty& property)
{
  const PropertyType& type = TypeOfValue(property.default_value);

  if (type.components == 1)
    shader_source.Write("{} {};\n", type.shader_type, property.name);
  else
    shader_source.Write("{}{} {};\n", type.shader_type, type.components, property.name);

  // Pad to a whole vec4 slot so each property starts on a 16-byte boundary.
  for (u32 slot = type.components + 1; slot <= 4; ++slot)
    shader_source.Write("{} {}_padding_{};\n", type.shader_type, property.name, slot);
}

CustomAssetLibrary::LoadInfo
RasterSurfaceShaderAsset::LoadImpl(const CustomAssetLibrary::AssetID& asset_id)
{
  return ReadAndPublish([&](RasterSurfaceShaderData* data) {
    return m_owning_library->LoadRasterSurfaceShader(asset_id, data);
  });
}
}  // namespace VideoCommon
