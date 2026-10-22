// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include <picojson.h>

#include "VideoCommon/Assets/CustomAssetLibrary.h"
#include "VideoCommon/Assets/Types.h"

// An asset declared by a graphics mod: {"name": "<asset id>", "data": {"<role>": "<file>"}}.
struct GraphicsModAssetConfig
{
  VideoCommon::CustomAssetLibrary::AssetID m_asset_id;
  VideoCommon::Assets::AssetMap m_map;

  void SerializeToConfig(picojson::object& json_obj) const;
  bool DeserializeFromConfig(const picojson::object& obj);
};
