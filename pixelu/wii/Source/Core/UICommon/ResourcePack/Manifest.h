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

  bool IsValid() const;
  bool IsCompressed() const;

  const std::string& GetName() const;
  const std::string& GetVersion() const;
  const std::string& GetID() const;
  const std::string& GetError() const;

  // Authors are joined with ", ".
  const std::optional<std::string>& GetAuthors() const;
  const std::optional<std::string>& GetDescription() const;
  const std::optional<std::string>& GetWebsite() const;

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
