// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Assets/TextureSamplerValue.h"

#include "Common/JsonUtil.h"
#include "Common/StringUtil.h"

namespace VideoCommon
{
namespace
{
constexpr const char* ORIGIN_ASSET = "asset";
constexpr const char* ORIGIN_TEXTURE_HASH = "texture_hash";
}  // namespace

std::string TextureSamplerValue::ToString(SamplerOrigin sampler_origin)
{
  return sampler_origin == SamplerOrigin::Asset ? ORIGIN_ASSET : ORIGIN_TEXTURE_HASH;
}

bool TextureSamplerValue::FromJson(const picojson::object& json, TextureSamplerValue* data)
{
  data->asset = ReadStringFromJson(json, "asset").value_or("");
  data->texture_hash = ReadStringFromJson(json, "texture_hash").value_or("");

  // The origin name is case-insensitive; anything unrecognised means "asset".
  std::string origin = ReadStringFromJson(json, "sampler_origin").value_or("");
  Common::ToLower(&origin);
  data->sampler_origin =
      origin == ORIGIN_TEXTURE_HASH ? SamplerOrigin::TextureHash : SamplerOrigin::Asset;
  return true;
}

void TextureSamplerValue::ToJson(picojson::object* obj, const TextureSamplerValue& data)
{
  if (obj == nullptr) [[unlikely]]
    return;

  obj->emplace("asset", data.asset);
  obj->emplace("texture_hash", data.texture_hash);
  obj->emplace("sampler_origin", ToString(data.sampler_origin));
}
}  // namespace VideoCommon
