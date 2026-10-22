// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <picojson.h>

#include "VideoCommon/GraphicsModSystem/Config/GraphicsModAsset.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsModFeature.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsTargetGroup.h"

// One graphics mod as described by its JSON file: metadata, target groups, features and
// assets. m_enabled and m_weight come from the user's per-game profile instead.
struct GraphicsModConfig
{
  std::string m_title;
  std::string m_author;
  std::string m_description;
  bool m_enabled = false;
  u16 m_weight = 0;
  // Path of the mod file below the directory its source keeps mods in.
  std::string m_relative_path;

  enum class Source
  {
    User,
    System
  };
  Source m_source = Source::User;

  std::vector<GraphicsTargetGroupConfig> m_groups;
  std::vector<GraphicsModFeatureConfig> m_features;
  std::vector<GraphicsModAssetConfig> m_assets;

  // Loads the mod file at `file`, which must lie below the source's mod directory.
  static std::optional<GraphicsModConfig> Create(const std::string& file, Source source);
  // Loads the mod a profile entry ({"source": .., "path": ..}) refers to.
  static std::optional<GraphicsModConfig> Create(const picojson::object* obj);

  std::string GetAbsolutePath() const;

  void SerializeToConfig(picojson::object& json_obj) const;
  bool DeserializeFromConfig(const picojson::value& value);

  void SerializeToProfile(picojson::object* value) const;
  void DeserializeFromProfile(const picojson::object& value);
};
