// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include <picojson.h>

// A feature of a graphics mod: apply action `m_action` (configured by `m_action_data`) to every
// target of the group named `m_group`.
struct GraphicsModFeatureConfig
{
  std::string m_group;
  std::string m_action;
  picojson::value m_action_data;

  void SerializeToConfig(picojson::object& json_obj) const;
  bool DeserializeFromConfig(const picojson::object& value);

  // Features have no per-user state; these exist for symmetry with groups and are no-ops.
  void SerializeToProfile(picojson::object* value) const;
  void DeserializeFromProfile(const picojson::object& value);
};
