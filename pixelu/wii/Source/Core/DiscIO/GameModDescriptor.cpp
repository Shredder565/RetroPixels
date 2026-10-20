// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/GameModDescriptor.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <picojson.h>

#include "Common/IOFile.h"
#include "Common/MathUtil.h"
#include "Common/StringUtil.h"

namespace DiscIO
{
namespace
{
constexpr std::string_view DESCRIPTOR_TYPE = "dolphin-game-mod-descriptor";
constexpr double DESCRIPTOR_VERSION = 1.0;

// Paths in a descriptor are relative to the descriptor's folder (absolute ones stay as they are).
std::string ResolvePath(const std::string& base_directory, const std::string& path)
{
  return PathToString(StringToPath(base_directory) / StringToPath(path));
}

// Returns the string value, or nullptr if the value is not a string.
const std::string* AsString(const picojson::value& value)
{
  return value.is<std::string>() ? &value.get<std::string>() : nullptr;
}

std::vector<GameModDescriptorRiivolutionPatchOption> ReadOptions(const picojson::array& json)
{
  std::vector<GameModDescriptorRiivolutionPatchOption> options;
  for (const picojson::value& item : json)
  {
    if (!item.is<picojson::object>())
      continue;

    GameModDescriptorRiivolutionPatchOption& option = options.emplace_back();
    for (const auto& [key, value] : item.get<picojson::object>())
    {
      if (const std::string* text = AsString(value))
      {
        if (key == "section-name")
          option.section_name = *text;
        else if (key == "option-id")
          option.option_id = *text;
        else if (key == "option-name")
          option.option_name = *text;
      }
      else if (key == "choice" && value.is<double>())
      {
        option.choice = MathUtil::SaturatingCast<u32>(value.get<double>());
      }
    }
  }
  return options;
}

GameModDescriptorRiivolution ReadRiivolution(const std::string& base_directory,
                                             const picojson::object& json)
{
  GameModDescriptorRiivolution riivolution;
  const auto patches = json.find("patches");
  if (patches == json.end() || !patches->second.is<picojson::array>())
    return riivolution;

  for (const picojson::value& item : patches->second.get<picojson::array>())
  {
    if (!item.is<picojson::object>())
      continue;

    GameModDescriptorRiivolutionPatch& patch = riivolution.patches.emplace_back();
    for (const auto& [key, value] : item.get<picojson::object>())
    {
      if (const std::string* text = AsString(value))
      {
        if (key == "xml")
          patch.xml = ResolvePath(base_directory, *text);
        else if (key == "root")
          patch.root = ResolvePath(base_directory, *text);
      }
      else if (key == "options" && value.is<picojson::array>())
      {
        patch.options = ReadOptions(value.get<picojson::array>());
      }
    }
  }
  return riivolution;
}

picojson::object WriteRiivolution(const GameModDescriptorRiivolution& riivolution)
{
  picojson::array patches;
  for (const GameModDescriptorRiivolutionPatch& patch : riivolution.patches)
  {
    picojson::object json_patch;
    if (!patch.xml.empty())
      json_patch.emplace("xml", picojson::value(patch.xml));
    if (!patch.root.empty())
      json_patch.emplace("root", picojson::value(patch.root));
    if (!patch.options.empty())
    {
      picojson::array options;
      for (const GameModDescriptorRiivolutionPatchOption& option : patch.options)
      {
        picojson::object json_option;
        if (!option.section_name.empty())
          json_option.emplace("section-name", picojson::value(option.section_name));
        if (!option.option_id.empty())
          json_option.emplace("option-id", picojson::value(option.option_id));
        if (!option.option_name.empty())
          json_option.emplace("option-name", picojson::value(option.option_name));
        json_option.emplace("choice", picojson::value(static_cast<double>(option.choice)));
        options.emplace_back(std::move(json_option));
      }
      json_patch.emplace("options", picojson::value(std::move(options)));
    }
    patches.emplace_back(std::move(json_patch));
  }

  picojson::object json;
  json.emplace("patches", picojson::value(std::move(patches)));
  return json;
}
}  // namespace

std::optional<GameModDescriptor> ParseGameModDescriptorFile(const std::string& filename)
{
  File::IOFile file(filename, "rb");
  if (!file)
    return std::nullopt;

  std::string contents(file.GetSize(), '\0');
  if (!file.ReadBytes(contents.data(), contents.size()))
    return std::nullopt;

#ifdef _WIN32
  // The folder of the file is taken with forward slashes only.
  const std::string path = ReplaceAll(filename, "\\", "/");
#else
  const std::string& path = filename;
#endif
  return ParseGameModDescriptorString(contents, path);
}

std::optional<GameModDescriptor> ParseGameModDescriptorString(std::string_view json,
                                                              std::string_view json_path)
{
  std::string base_directory;
  SplitPath(json_path, &base_directory, nullptr, nullptr);

  picojson::value root;
  std::string error;
  picojson::parse(root, json.begin(), json.end(), &error);
  if (!error.empty() || !root.is<picojson::object>())
    return std::nullopt;

  GameModDescriptor descriptor;
  bool right_type = false;
  bool right_version = false;
  for (const auto& [key, value] : root.get<picojson::object>())
  {
    if (key == "version")
    {
      if (value.is<double>())
        right_version = value.get<double>() == DESCRIPTOR_VERSION;
    }
    else if (key == "riivolution")
    {
      if (value.is<picojson::object>())
        descriptor.riivolution = ReadRiivolution(base_directory, value.get<picojson::object>());
    }
    else if (const std::string* text = AsString(value))
    {
      if (key == "type")
        right_type = *text == DESCRIPTOR_TYPE;
      else if (key == "base-file")
        descriptor.base_file = ResolvePath(base_directory, *text);
      else if (key == "display-name")
        descriptor.display_name = *text;
      else if (key == "maker")
        descriptor.maker = *text;
      else if (key == "banner")
        descriptor.banner = ResolvePath(base_directory, *text);
    }
  }

  if (!right_type || !right_version)
    return std::nullopt;
  return descriptor;
}

std::string WriteGameModDescriptorString(const GameModDescriptor& descriptor, bool pretty)
{
  picojson::object json;
  json.emplace("type", picojson::value(std::string(DESCRIPTOR_TYPE)));
  json.emplace("version", picojson::value(DESCRIPTOR_VERSION));
  if (!descriptor.base_file.empty())
    json.emplace("base-file", picojson::value(descriptor.base_file));
  if (!descriptor.display_name.empty())
    json.emplace("display-name", picojson::value(descriptor.display_name));
  if (!descriptor.maker.empty())
    json.emplace("maker", picojson::value(descriptor.maker));
  if (!descriptor.banner.empty())
    json.emplace("banner", picojson::value(descriptor.banner));
  if (descriptor.riivolution)
    json.emplace("riivolution", picojson::value(WriteRiivolution(*descriptor.riivolution)));
  return picojson::value(json).serialize(pretty);
}

bool WriteGameModDescriptorFile(const std::string& filename, const GameModDescriptor& descriptor,
                                bool pretty)
{
  const std::string json = WriteGameModDescriptorString(descriptor, pretty);
  if (json.empty())
    return false;

  File::IOFile file(filename, "wb");
  return file && file.WriteString(json);
}
}  // namespace DiscIO
