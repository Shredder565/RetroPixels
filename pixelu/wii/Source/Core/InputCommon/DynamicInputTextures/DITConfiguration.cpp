// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/DynamicInputTextures/DITConfiguration.h"

#include <limits>
#include <optional>
#include <string>
#include <utility>

#include <picojson.h>

#include "Common/IniFile.h"
#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "InputCommon/DynamicInputTextures/DITSpecification.h"
#include "InputCommon/ImageOperations.h"

namespace InputCommon::DynamicInputTextures
{
namespace
{
// The only pack format there is.
constexpr u8 SUPPORTED_SPECIFICATION = 1;

// The host key image scaled to fill one region.
ImagePixelData FitToRegion(const ImagePixelData& key_image, const Rect& region,
                           bool keep_aspect_ratio)
{
  const u32 width = region.GetWidth();
  const u32 height = region.GetHeight();

  if (key_image.width == width && key_image.height == height)
    return key_image;
  if (keep_aspect_ratio)
    return ResizeKeepAspectRatio(ResizeMode::Nearest, key_image, width, height, Pixel{0, 0, 0, 0});
  return Resize(ResizeMode::Nearest, key_image, width, height);
}
}  // namespace

Configuration::Configuration(const std::string& json_path)
{
  // Only a fully read pack is valid.
  m_valid = false;

  std::string parse_error;
  picojson::value root;
  if (!JsonFromFile(json_path, &root, &parse_error))
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load dynamic input json file '{}' due to parse error: {}",
                  json_path, parse_error);
    return;
  }

  // Image names in the pack are relative to its folder.
  SplitPath(json_path, &m_base_path, nullptr, nullptr);

  // "specification" is optional and defaults to 1.
  u8 specification = SUPPORTED_SPECIFICATION;
  const picojson::value& specification_json = root.get("specification");
  if (specification_json.is<double>())
  {
    const double number = specification_json.get<double>();
    constexpr double lowest = std::numeric_limits<u8>::min();
    constexpr double highest = std::numeric_limits<u8>::max();
    if (number < lowest || number > highest)
    {
      ERROR_LOG_FMT(
          VIDEO,
          "Failed to load dynamic input json file '{}', specification '{}' is not within bounds",
          json_path, number);
      return;
    }
    specification = static_cast<u8>(number);
  }

  if (specification != SUPPORTED_SPECIFICATION)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load dynamic input json file '{}', specification '{}' is invalid",
                  json_path, specification);
    return;
  }

  m_valid = ProcessSpecificationV1(root, m_dynamic_input_textures, m_base_path, json_path);
}

Configuration::~Configuration() = default;

void Configuration::GenerateTexture(const Common::IniFile& mappings,
                                    const std::vector<std::string>& controller_names,
                                    const Data& texture_data, OutputDetails* textures) const
{
  // Regions whose key has no image keep the base image's pixels.
  std::optional<ImagePixelData> canvas = LoadImage(m_base_path + texture_data.m_image_name);
  if (!canvas)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load image '{}' needed for dynamic input texture generation",
                  texture_data.m_image_name);
    return;
  }

  // Set when anything about the mappings differs from the base image: a key drawn, a key
  // without an image, or a device the pack does not know.
  bool changed = false;

  for (const std::string& controller_name : controller_names)
  {
    const Common::IniFile::Section* const section = mappings.GetSection(controller_name);
    if (section == nullptr)
      continue;

    std::string device;
    if (!section->Get("Device", &device))
      continue;

    const auto emulated = texture_data.m_emulated_controllers.find(controller_name);
    if (emulated == texture_data.m_emulated_controllers.end())
      continue;

    // An exact device match, else the pack's catch-all entry ("").
    auto host = texture_data.m_host_devices.find(device);
    if (host == texture_data.m_host_devices.end())
      host = texture_data.m_host_devices.find("");
    const bool known_device = host != texture_data.m_host_devices.end();

    for (const auto& [emulated_control, regions] : emulated->second)
    {
      if (!known_device)
      {
        changed = true;
        continue;
      }

      std::string host_control;
      section->Get(emulated_control, &host_control);

      const auto key_image_name = host->second.find(host_control);
      if (key_image_name == host->second.end())
      {
        changed = true;
        continue;
      }

      const std::string key_image_path = m_base_path + key_image_name->second;
      const std::optional<ImagePixelData> key_image = LoadImage(key_image_path);
      if (!key_image)
      {
        ERROR_LOG_FMT(VIDEO,
                      "Failed to load image '{}' needed for dynamic input texture generation",
                      key_image_path);
        continue;
      }

      for (const Rect& region : regions)
      {
        const ImagePixelData scaled =
            FitToRegion(*key_image, region, texture_data.m_preserve_aspect_ratio);
        CopyImageRegion(scaled, *canvas, Rect{0, 0, region.GetWidth(), region.GetHeight()},
                        region);
        changed = true;
      }
    }
  }

  if (changed)
  {
    (*textures)[texture_data.m_generated_folder_name][texture_data.m_hires_texture_name] =
        std::move(*canvas);
  }
}

void Configuration::GenerateTextures(const Common::IniFile& mappings,
                                     const std::vector<std::string>& controller_names,
                                     OutputDetails* textures) const
{
  for (const Data& texture : m_dynamic_input_textures)
    GenerateTexture(mappings, controller_names, texture, textures);
}
}  // namespace InputCommon::DynamicInputTextures
