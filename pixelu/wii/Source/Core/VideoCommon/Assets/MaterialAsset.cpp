// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Assets/MaterialAsset.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "VideoCommon/Assets/CustomAssetLibrary.h"

namespace VideoCommon
{
namespace
{
using Value = MaterialProperty::Value;

// Every property takes one std140 vec4 slot whatever its type.
constexpr std::size_t SLOT_SIZE = 4 * sizeof(float);

// JSON type names, in the order of Value's alternatives.
constexpr std::array<std::string_view, 9> TYPE_NAMES{"int",    "int2",   "int3",   "int4", "float",
                                                     "float2", "float3", "float4", "bool"};
static_assert(std::variant_size_v<Value> == TYPE_NAMES.size());

template <typename T>
struct ArrayTraits
{
  static constexpr bool is_array = false;
};
template <typename E, std::size_t N>
struct ArrayTraits<std::array<E, N>>
{
  static constexpr bool is_array = true;
  using Element = E;
  static constexpr std::size_t count = N;
};

template <typename Number>
bool ReadScalar(const CustomAssetLibrary::AssetID& asset_id, const picojson::value& json,
                Value* out)
{
  if (!json.is<double>())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Asset id '{}' material has attribute where a double was expected but not "
                  "provided.",
                  asset_id);
    return false;
  }
  *out = static_cast<Number>(json.get<double>());
  return true;
}

template <typename Array>
bool ReadArray(const CustomAssetLibrary::AssetID& asset_id, const picojson::value& json, Value* out)
{
  using Traits = ArrayTraits<Array>;
  if (!json.is<picojson::array>())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Asset id '{}' material has attribute where an array was expected but not "
                  "provided.",
                  asset_id);
    return false;
  }

  const picojson::array& items = json.get<picojson::array>();
  if (items.size() != Traits::count)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Asset id '{}' material has attribute with incorrect number of elements, "
                  "expected {}",
                  asset_id, Traits::count);
    return false;
  }
  if (!std::ranges::all_of(items, [](const picojson::value& v) { return v.is<double>(); }))
  {
    ERROR_LOG_FMT(VIDEO,
                  "Asset id '{}' material has attribute where all elements are not of type "
                  "double.",
                  asset_id);
    return false;
  }

  Array numbers;
  for (std::size_t i = 0; i < Traits::count; ++i)
    numbers[i] = static_cast<typename Traits::Element>(items[i].get<double>());
  *out = numbers;
  return true;
}

void ReportBadValue(const CustomAssetLibrary::AssetID& asset_id, std::string_view type)
{
  ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse the json, value is not valid for type '{}'",
                asset_id, type);
}

template <std::size_t I>
bool ReadAlternative(const CustomAssetLibrary::AssetID& asset_id, const picojson::value& json,
                     Value* out)
{
  using Alt = std::variant_alternative_t<I, Value>;
  if constexpr (std::is_same_v<Alt, bool>)
  {
    if (!json.is<bool>())
    {
      ReportBadValue(asset_id, TYPE_NAMES[I]);
      return false;
    }
    *out = json.get<bool>();
    return true;
  }
  else if constexpr (ArrayTraits<Alt>::is_array)
  {
    return ReadArray<Alt>(asset_id, json, out);
  }
  else
  {
    return ReadScalar<Alt>(asset_id, json, out);
  }
}

template <std::size_t... I>
bool ReadValueAt(std::size_t index, const CustomAssetLibrary::AssetID& asset_id,
                 const picojson::value& json, Value* out, std::index_sequence<I...>)
{
  bool ok = false;
  (void)((index == I ? (ok = ReadAlternative<I>(asset_id, json, out), true) : false) || ...);
  return ok;
}

bool ReadValue(const CustomAssetLibrary::AssetID& asset_id, const picojson::value& json,
               std::string_view type, Value* out)
{
  const auto known = std::ranges::find(TYPE_NAMES, type);
  if (known == TYPE_NAMES.end())
  {
    ReportBadValue(asset_id, type);
    return false;
  }
  return ReadValueAt(static_cast<std::size_t>(known - TYPE_NAMES.begin()), asset_id, json, out,
                     std::make_index_sequence<TYPE_NAMES.size()>{});
}

// A property without a "value" keeps the default (an int 0), whatever its declared type.
bool ReadProperty(const CustomAssetLibrary::AssetID& asset_id, const picojson::value& json,
                  MaterialProperty* property)
{
  if (!json.is<picojson::object>())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse the json, value is not the right json type",
                  asset_id);
    return false;
  }
  const picojson::object& obj = json.get<picojson::object>();

  const auto type_entry = obj.find("type");
  if (type_entry == obj.end())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse the json, value entry 'type' not found",
                  asset_id);
    return false;
  }
  if (!type_entry->second.is<std::string>())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Asset '{}' failed to parse the json, value entry 'type' is not the right json "
                  "type",
                  asset_id);
    return false;
  }
  std::string type = type_entry->second.to_str();
  Common::ToLower(&type);

  const auto value_entry = obj.find("value");
  if (value_entry == obj.end())
    return true;
  return ReadValue(asset_id, value_entry->second, type, &property->m_value);
}

u32 ReadFlag(const picojson::object& obj, const std::string& key)
{
  return ReadNumericFromJson<u32>(obj, key).value_or(0);
}

template <typename Enum>
Enum ReadEnum(const picojson::object& obj, const std::string& key, Enum fallback)
{
  return ReadNumericFromJson<Enum>(obj, key).value_or(fallback);
}

const picojson::object* FindObject(const picojson::object& json, const std::string& key)
{
  const auto it = json.find(key);
  if (it == json.end() || !it->second.is<picojson::object>())
    return nullptr;
  return &it->second.get<picojson::object>();
}

DepthState ReadDepthState(const picojson::object& obj)
{
  DepthState state;
  state.test_enable = ReadFlag(obj, "test_enable");
  state.update_enable = ReadFlag(obj, "update_enable");
  state.func = ReadEnum(obj, "func", CompareMode::Never);
  return state;
}

BlendingState ReadBlendingState(const picojson::object& obj)
{
  BlendingState state;
  state.blend_enable = ReadFlag(obj, "blend_enable");
  state.logic_op_enable = ReadFlag(obj, "logic_op_enable");
  state.color_update = ReadFlag(obj, "color_update");
  state.alpha_update = ReadFlag(obj, "alpha_update");
  state.subtract = ReadFlag(obj, "subtract");
  state.subtract_alpha = ReadFlag(obj, "subtract_alpha");
  state.use_dual_src = ReadFlag(obj, "use_dual_src");
  state.dst_factor = ReadEnum(obj, "dst_factor", DstBlendFactor::Zero);
  state.src_factor = ReadEnum(obj, "src_factor", SrcBlendFactor::Zero);
  state.dst_factor_alpha = ReadEnum(obj, "dst_factor_alpha", DstBlendFactor::Zero);
  state.src_factor_alpha = ReadEnum(obj, "src_factor_alpha", SrcBlendFactor::Zero);
  state.logic_mode = ReadEnum(obj, "logic_mode", LogicOp::Clear);
  return state;
}

template <typename Field>
picojson::value Number(const Field& field)
{
  return picojson::value(static_cast<double>(field.Value()));
}

picojson::object DepthStateToJson(const DepthState& state)
{
  picojson::object obj;
  obj.emplace("test_enable", Number(state.test_enable));
  obj.emplace("update_enable", Number(state.update_enable));
  obj.emplace("func", Number(state.func));
  return obj;
}

picojson::object BlendingStateToJson(const BlendingState& state)
{
  picojson::object obj;
  obj.emplace("blend_enable", Number(state.blend_enable));
  obj.emplace("logic_op_enable", Number(state.logic_op_enable));
  obj.emplace("color_update", Number(state.color_update));
  obj.emplace("alpha_update", Number(state.alpha_update));
  obj.emplace("subtract", Number(state.subtract));
  obj.emplace("subtract_alpha", Number(state.subtract_alpha));
  obj.emplace("use_dual_src", Number(state.use_dual_src));
  obj.emplace("dst_factor", Number(state.dst_factor));
  obj.emplace("src_factor", Number(state.src_factor));
  obj.emplace("dst_factor_alpha", Number(state.dst_factor_alpha));
  obj.emplace("src_factor_alpha", Number(state.src_factor_alpha));
  obj.emplace("logic_mode", Number(state.logic_mode));
  return obj;
}

template <typename T>
picojson::value ValueToJson(const T& value)
{
  if constexpr (std::is_same_v<T, bool>)
    return picojson::value(value);
  else if constexpr (ArrayTraits<T>::is_array)
    return picojson::value(ToJsonArray(value));
  else
    return picojson::value(static_cast<double>(value));
}
}  // namespace

void MaterialProperty::WriteToMemory(u8*& buffer, const MaterialProperty& property)
{
  std::memset(buffer, 0, SLOT_SIZE);
  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, bool>)
        {
          // Shaders see a bool as a 32-bit integer.
          const u32 as_int = value ? 1 : 0;
          std::memcpy(buffer, &as_int, sizeof(as_int));
        }
        else if constexpr (ArrayTraits<T>::is_array)
        {
          std::memcpy(buffer, value.data(), sizeof(value));
        }
        else
        {
          std::memcpy(buffer, &value, sizeof(value));
        }
      },
      property.m_value);
  buffer += SLOT_SIZE;
}

std::size_t MaterialProperty::GetMemorySize(const MaterialProperty&)
{
  return SLOT_SIZE;
}

bool MaterialData::FromJson(const CustomAssetLibrary::AssetID& asset_id,
                            const picojson::object& json, MaterialData* data)
{
  const auto properties = json.find("properties");
  if (properties == json.end())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, '{}' not found", asset_id, "properties");
    return false;
  }
  if (!properties->second.is<picojson::array>())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, '{}' is not the right json type",
                  asset_id, "properties");
    return false;
  }
  for (const picojson::value& entry : properties->second.get<picojson::array>())
  {
    MaterialProperty property;
    if (!ReadProperty(asset_id, entry, &property))
      return false;
    data->properties.push_back(std::move(property));
  }

  const std::optional<std::string> shader = ReadStringFromJson(json, "shader_asset");
  if (!shader)
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, 'shader_asset' not found or wrong type",
                  asset_id);
    return false;
  }
  data->shader_asset = *shader;

  const std::optional<std::string> next = ReadStringFromJson(json, "next_material_asset");
  if (!next)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Asset '{}' failed to parse json, 'next_material_asset' not found or wrong type",
                  asset_id);
    return false;
  }
  data->next_material_asset = *next;

  // Render state overrides are optional; a malformed one is ignored.
  if (const std::optional<CullMode> cull = ReadNumericFromJson<CullMode>(json, "cull_mode"))
    data->cull_mode = *cull;
  if (const picojson::object* depth = FindObject(json, "depth_state"))
    data->depth_state = ReadDepthState(*depth);
  if (const picojson::object* blend = FindObject(json, "blending_state"))
    data->blending_state = ReadBlendingState(*blend);

  const auto textures = json.find("textures");
  if (textures == json.end())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, 'textures' not found", asset_id);
    return false;
  }
  if (!textures->second.is<picojson::array>())
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, 'textures' is not the right json type",
                  asset_id);
    return false;
  }
  const picojson::array& texture_entries = textures->second.get<picojson::array>();
  if (!std::ranges::all_of(texture_entries,
                           [](const picojson::value& v) { return v.is<picojson::object>(); }))
  {
    ERROR_LOG_FMT(VIDEO, "Asset '{}' failed to parse json, 'textures' must contain objects",
                  asset_id);
    return false;
  }
  for (const picojson::value& entry : texture_entries)
  {
    TextureSamplerValue texture;
    if (!TextureSamplerValue::FromJson(entry.get<picojson::object>(), &texture))
      return false;
    data->textures.push_back(std::move(texture));
  }

  return true;
}

void MaterialData::ToJson(picojson::object* obj, const MaterialData& data)
{
  if (obj == nullptr) [[unlikely]]
    return;

  picojson::array properties;
  for (const MaterialProperty& property : data.properties)
  {
    picojson::object entry;
    entry["type"] = picojson::value(std::string{TYPE_NAMES[property.m_value.index()]});
    entry["value"] = std::visit([](const auto& v) { return ValueToJson(v); }, property.m_value);
    properties.emplace_back(std::move(entry));
  }
  obj->emplace("properties", std::move(properties));

  obj->emplace("shader_asset", data.shader_asset);
  obj->emplace("next_material_asset", data.next_material_asset);

  if (data.cull_mode)
    obj->emplace("cull_mode", static_cast<double>(*data.cull_mode));
  if (data.depth_state)
    obj->emplace("depth_state", DepthStateToJson(*data.depth_state));
  if (data.blending_state)
    obj->emplace("blending_state", BlendingStateToJson(*data.blending_state));

  picojson::array textures;
  for (const TextureSamplerValue& texture : data.textures)
  {
    picojson::object entry;
    TextureSamplerValue::ToJson(&entry, texture);
    textures.emplace_back(std::move(entry));
  }
  obj->emplace("textures", std::move(textures));
}

CustomAssetLibrary::LoadInfo MaterialAsset::LoadImpl(const CustomAssetLibrary::AssetID& asset_id)
{
  return ReadAndPublish(
      [&](MaterialData* data) { return m_owning_library->LoadMaterial(asset_id, data); });
}
}  // namespace VideoCommon
