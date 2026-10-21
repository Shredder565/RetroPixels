// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "InputCommon/ImageOperations.h"

namespace InputCommon::DynamicInputTextures
{
// One output texture of a dynamic input texture pack.
struct Data
{
  // The base image (relative to the pack's JSON file).
  std::string m_image_name;
  // The hi-res texture file name to write.
  std::string m_hires_texture_name;
  // The hi-res texture folder to write it to.
  std::string m_generated_folder_name;

  // Emulated controller -> emulated control -> regions of the base image to draw into.
  using EmulatedKeyToRegionsMap = std::unordered_map<std::string, std::vector<Rect>>;
  std::unordered_map<std::string, EmulatedKeyToRegionsMap> m_emulated_controllers;

  // Host device ("" for any device) -> host control -> image to draw.
  using HostKeyToImagePath = std::unordered_map<std::string, std::string>;
  std::unordered_map<std::string, HostKeyToImagePath> m_host_devices;
  bool m_preserve_aspect_ratio = true;
};
}  // namespace InputCommon::DynamicInputTextures
