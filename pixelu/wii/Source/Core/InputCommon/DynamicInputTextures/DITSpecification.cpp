// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/DynamicInputTextures/DITSpecification.h"

#include <algorithm>
#include <utility>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/ConfigManager.h"

// Version 1 pack layout:
//   preserve_aspect_ratio (bool, default true), generated_folder_name (string, default
//   "<game id>_Generated"), default_host_controls (object),
//   output_textures: { <texture name>: { image, emulated_controls, host_controls? } }
// emulated_controls maps controller -> control -> list of [left, top, right, bottom] regions;
// host_controls maps host device -> host control -> key image.
namespace InputCommon::DynamicInputTextures
{
namespace
{
// Settings from the top level that every texture of the pack shares.
struct SharedSettings
{
  bool preserve_aspect_ratio = true;
  std::string generated_folder_name;
  picojson::object host_controls;
};

SharedSettings ReadSharedSettings(const picojson::value& root)
{
  SharedSettings shared;

  const picojson::value& aspect = root.get("preserve_aspect_ratio");
  if (aspect.is<bool>())
    shared.preserve_aspect_ratio = aspect.get<bool>();

  const picojson::value& folder = root.get("generated_folder_name");
  const std::string& game_id = SConfig::GetInstance().GetGameID();
  shared.generated_folder_name = fmt::format("{}_Generated", game_id);
  if (folder.is<std::string>())
    shared.generated_folder_name = folder.get<std::string>();

  const picojson::value& host_controls = root.get("default_host_controls");
  if (host_controls.is<picojson::object>())
    shared.host_controls = host_controls.get<picojson::object>();

  return shared;
}

// One [left, top, right, bottom] entry.
bool ReadRegion(const picojson::value& value, const std::string& json_file,
                const std::string& controller, const std::string& control, Rect* region)
{
  if (!value.is<picojson::array>())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load dynamic input json file '{}' because emulated controller '{}' "
                  "key '{}' has a region with the incorrect type.  Expected array ",
                  json_file, controller, control);
    return false;
  }

  const picojson::array& edges = value.get<picojson::array>();
  if (edges.size() != 4)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load dynamic input json file '{}' because emulated controller '{}' "
                  "key '{}' has a region that does not have 4 offsets (left, top, right, "
                  "bottom).",
                  json_file, controller, control);
    return false;
  }

  const bool all_numbers =
      std::ranges::all_of(edges, [](const picojson::value& edge) { return edge.is<double>(); });
  if (!all_numbers)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load dynamic input json file '{}' because emulated controller '{}' "
                  "key '{}' has a region that has the incorrect offset type.",
                  json_file, controller, control);
    return false;
  }

  region->left = static_cast<u32>(edges[0].get<double>());
  region->top = static_cast<u32>(edges[1].get<double>());
  region->right = static_cast<u32>(edges[2].get<double>());
  region->bottom = static_cast<u32>(edges[3].get<double>());
  return true;
}

// The "emulated_controls" object of one texture.
bool ReadEmulatedControls(const picojson::object& controllers, const std::string& json_file,
                          Data* texture)
{
  for (const auto& [controller, controls] : controllers)
  {
    if (!controls.is<picojson::object>())
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load dynamic input json file '{}' because 'emulated_controls' "
                    "map key '{}' is incorrect type.  Expected map  ",
                    json_file, controller);
      return false;
    }

    Data::EmulatedKeyToRegionsMap& destination = texture->m_emulated_controllers[controller];
    for (const auto& [control, region_list] : controls.get<picojson::object>())
    {
      if (!region_list.is<picojson::array>())
      {
        ERROR_LOG_FMT(VIDEO,
                      "Failed to load dynamic input json file '{}' because emulated controller "
                      "'{}' key '{}' has incorrect value type.  Expected array ",
                      json_file, controller, control);
        return false;
      }

      std::vector<Rect> regions;
      for (const picojson::value& entry : region_list.get<picojson::array>())
      {
        Rect region;
        if (!ReadRegion(entry, json_file, controller, control, &region))
          return false;
        regions.push_back(region);
      }
      destination.insert_or_assign(control, std::move(regions));
    }
  }
  return true;
}

// The host controls of one texture (its own, else the pack's defaults).
bool ReadHostControls(const picojson::object& devices, const std::string& json_file,
                      Data* texture)
{
  if (devices.empty())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load dynamic input json file '{}' because field "
                  "'host_controls' is missing ",
                  json_file);
    return false;
  }

  for (const auto& [device, controls] : devices)
  {
    if (!controls.is<picojson::object>())
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load dynamic input json file '{}' because 'host_controls' "
                    "map key '{}' is incorrect type ",
                    json_file, device);
      return false;
    }

    Data::HostKeyToImagePath& destination = texture->m_host_devices[device];
    // Values that are not strings are taken in their JSON text form.
    for (const auto& [control, key_image] : controls.get<picojson::object>())
      destination.insert_or_assign(control, key_image.to_str());
  }
  return true;
}
}  // namespace

bool ProcessSpecificationV1(picojson::value& root, std::vector<Data>& input_textures,
                            const std::string& base_path, const std::string& json_file)
{
  const picojson::value& textures_json = root.get("output_textures");
  if (!textures_json.is<picojson::object>())
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load dynamic input json file '{}' because 'output_textures' is "
                  "missing or was not of type object",
                  json_file);
    return false;
  }

  const SharedSettings shared = ReadSharedSettings(root);

  for (const auto& [texture_name, texture_json] : textures_json.get<picojson::object>())
  {
    const picojson::value& image = texture_json.get("image");
    const picojson::value& emulated_controls = texture_json.get("emulated_controls");
    if (!image.is<std::string>() || !emulated_controls.is<picojson::object>())
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load dynamic input json file '{}' because required fields "
                    "'image', or 'emulated_controls' are either "
                    "missing or the incorrect type",
                    json_file);
      return false;
    }

    Data texture;
    texture.m_hires_texture_name = texture_name;
    texture.m_image_name = image.get<std::string>();
    texture.m_preserve_aspect_ratio = shared.preserve_aspect_ratio;
    texture.m_generated_folder_name = shared.generated_folder_name;

    const std::string image_path = base_path + texture.m_image_name;
    if (!File::Exists(image_path))
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load dynamic input json file '{}' because the image '{}' "
                    "could not be loaded",
                    json_file, image_path);
      return false;
    }

    if (!ReadEmulatedControls(emulated_controls.get<picojson::object>(), json_file, &texture))
      return false;

    const picojson::value& own_host_controls = texture_json.get("host_controls");
    const picojson::object& host_controls = own_host_controls.is<picojson::object>() ?
                                                own_host_controls.get<picojson::object>() :
                                                shared.host_controls;
    if (!ReadHostControls(host_controls, json_file, &texture))
      return false;

    input_textures.emplace_back(std::move(texture));
  }
  return true;
}
}  // namespace InputCommon::DynamicInputTextures
