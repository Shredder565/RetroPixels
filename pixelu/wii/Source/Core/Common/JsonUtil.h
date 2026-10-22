// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <iterator>
#include <optional>
#include <string>
#include <type_traits>

#include <picojson.h>

#include "Common/Matrix.h"

// Helpers around picojson. JSON numbers are doubles, so arithmetic values are converted to and
// from double.

// Converts a container (anything with value_type, size and begin/end) to a JSON array.
template <typename Range>
picojson::array ToJsonArray(const Range& data)
{
  using Element = typename Range::value_type;
  picojson::array array;
  array.reserve(std::size(data));
  for (const auto& element : data)
  {
    if constexpr (std::is_arithmetic_v<Element>)  // bool included: written as 0/1
      array.emplace_back(static_cast<double>(element));
    else
      array.emplace_back(element);
  }
  return array;
}

// obj[key] converted to Type, or nullopt if it is missing or not a number.
template <typename Type>
std::optional<Type> ReadNumericFromJson(const picojson::object& obj, const std::string& key)
{
  const auto found = obj.find(key);
  if (found == obj.end() || !found->second.is<double>())
    return std::nullopt;
  return static_cast<Type>(found->second.get<double>());
}

// obj[key] if it is present and a string / a boolean.
std::optional<std::string> ReadStringFromJson(const picojson::object& obj, const std::string& key);
std::optional<bool> ReadBoolFromJson(const picojson::object& obj, const std::string& key);

// {"x": .., "y": .., "z": ..}; missing or non-numeric members read as 0.
picojson::object ToJsonObject(const Common::Vec3& vec);
void FromJson(const picojson::object& obj, Common::Vec3& vec);

// Writes `root` to a file; false if the file could not be opened.
bool JsonToFile(const std::string& filename, const picojson::value& root, bool prettify = false);
// Parses a file into *root. Returns false if it could not be read (leaving *error untouched) or
// did not parse (with the parser's message in *error).
bool JsonFromFile(const std::string& filename, picojson::value* root, std::string* error);
