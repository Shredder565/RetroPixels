// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

#include <picojson.h>

#include "VideoCommon/GraphicsModSystem/Config/GraphicsTarget.h"

// A named list of targets that features refer to by name.
struct GraphicsTargetGroupConfig
{
  std::string m_name;
  std::vector<GraphicsTargetConfig> m_targets;

  void SerializeToConfig(picojson::object& json_obj) const;
  bool DeserializeFromConfig(const picojson::object& obj);

  void SerializeToProfile(picojson::object* obj) const;
  void DeserializeFromProfile(const picojson::object& obj);
};
