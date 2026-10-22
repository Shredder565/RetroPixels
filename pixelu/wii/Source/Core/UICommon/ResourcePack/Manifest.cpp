// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "UICommon/ResourcePack/Manifest.h"

#include <picojson.h>

namespace ResourcePack
{
namespace
{
std::optional<std::string> OptionalText(const picojson::value& field)
{
  if (!field.is<std::string>())
    return std::nullopt;
  return field.to_str();
}
}  // namespace

Manifest::Manifest(const std::string& text)
{
  picojson::value root;
  if (!picojson::parse(root, text).empty())
  {
    Fail("Failed to parse manifest.");
    return;
  }

  const picojson::value& name = root.get("name");
  const picojson::value& version = root.get("version");
  const picojson::value& id = root.get("id");
  const picojson::value& authors = root.get("authors");
  const picojson::value& description = root.get("description");
  const picojson::value& website = root.get("website");
  const picojson::value& compressed = root.get("compressed");

  const bool required_ok =
      name.is<std::string>() && version.is<std::string>() && id.is<std::string>();
  if (!required_ok)
  {
    Fail("Some objects have a bad type.");
    return;
  }

  m_name = name.to_str();
  m_version = version.to_str();
  m_id = id.to_str();

  // Array members are listed by their JSON text form (strings as themselves); an empty array
  // counts as no authors.
  if (authors.is<picojson::array>())
  {
    const picojson::array& list = authors.get<picojson::array>();
    if (!list.empty())
    {
      std::string joined = list.front().to_str();
      for (std::size_t i = 1; i < list.size(); ++i)
        joined += ", " + list[i].to_str();
      m_authors = std::move(joined);
    }
  }

  m_description = OptionalText(description);
  m_website = OptionalText(website);
  if (compressed.is<bool>())
    m_compressed = compressed.get<bool>();
}

void Manifest::Fail(const char* reason)
{
  m_valid = false;
  m_error = reason;
}

}  // namespace ResourcePack
