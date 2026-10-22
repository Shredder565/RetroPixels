// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>

namespace ResourcePack
{
// A resource pack's manifest.json. "name", "version" and "id" are required strings; "authors"
// (array), "description", "website" (strings) and "compressed" (bool) are optional.
class Manifest
{
public:
  explicit Manifest(const std::string& text);

  bool IsValid() const { return m_valid; }
  bool IsCompressed() const { return m_compressed; }

  const std::string& GetName() const { return m_name; }
  const std::string& GetVersion() const { return m_version; }
  const std::string& GetID() const { return m_id; }
  // Why the manifest is invalid.
  const std::string& GetError() const { return m_error; }

  // Authors are joined with ", ".
  const std::optional<std::string>& GetAuthors() const { return m_authors; }
  const std::optional<std::string>& GetDescription() const { return m_description; }
  const std::optional<std::string>& GetWebsite() const { return m_website; }

private:
  void Fail(const char* reason);

  bool m_valid = true;
  bool m_compressed = false;

  std::string m_name;
  std::string m_version;
  std::string m_id;
  std::string m_error;

  std::optional<std::string> m_authors;
  std::optional<std::string> m_description;
  std::optional<std::string> m_website;
};
}  // namespace ResourcePack
