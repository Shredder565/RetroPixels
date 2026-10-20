// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/JsonUtil.h"

#include <fstream>

#include "Common/FileUtil.h"

namespace
{
template <typename T>
std::optional<T> ReadTyped(const picojson::object& obj, const std::string& key)
{
  const auto found = obj.find(key);
  if (found == obj.end() || !found->second.is<T>())
    return std::nullopt;
  return found->second.get<T>();
}
}  // namespace

picojson::object ToJsonObject(const Common::Vec3& vec)
{
  return picojson::object{
      {"x", picojson::value(static_cast<double>(vec.x))},
      {"y", picojson::value(static_cast<double>(vec.y))},
      {"z", picojson::value(static_cast<double>(vec.z))},
  };
}

void FromJson(const picojson::object& obj, Common::Vec3& vec)
{
  vec.x = ReadNumericFromJson<float>(obj, "x").value_or(0.0f);
  vec.y = ReadNumericFromJson<float>(obj, "y").value_or(0.0f);
  vec.z = ReadNumericFromJson<float>(obj, "z").value_or(0.0f);
}

std::optional<std::string> ReadStringFromJson(const picojson::object& obj, const std::string& key)
{
  return ReadTyped<std::string>(obj, key);
}

std::optional<bool> ReadBoolFromJson(const picojson::object& obj, const std::string& key)
{
  return ReadTyped<bool>(obj, key);
}

bool JsonToFile(const std::string& filename, const picojson::value& root, bool prettify)
{
  std::ofstream out;
  File::OpenFStream(out, filename, std::ios_base::out);
  if (!out.is_open())
    return false;
  out << root.serialize(prettify);
  return true;
}

bool JsonFromFile(const std::string& filename, picojson::value* root, std::string* error)
{
  std::string text;
  if (!File::ReadFileToString(filename, text))
    return false;
  *error = picojson::parse(*root, text);
  return error->empty();
}
