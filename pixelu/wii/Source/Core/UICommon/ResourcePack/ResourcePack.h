// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

#include "UICommon/ResourcePack/Manifest.h"

namespace ResourcePack
{
// A texture pack in a .zip archive: manifest.json, an optional logo.png, and replacement
// textures under textures/. Installing copies the textures into the user's hi-res texture
// folder, leaving alone those that a higher-priority pack provides.
class ResourcePack
{
public:
  explicit ResourcePack(const std::string& path);

  bool IsValid() const { return m_valid; }
  // The bytes of logo.png; empty if the pack has none.
  const std::vector<char>& GetLogo() const { return m_logo_data; }

  const std::string& GetPath() const { return m_path; }
  const std::string& GetError() const { return m_error; }
  // Null when the archive or its manifest.json could not be read.
  const Manifest* GetManifest() const { return m_manifest.get(); }
  // Texture paths relative to textures/.
  const std::vector<std::string>& GetTextures() const { return m_textures; }

  // `path` is the user folder.
  bool Install(const std::string& path);
  bool Uninstall(const std::string& path);

  // Packs are identified by their archive path.
  bool operator==(const ResourcePack& pack) const;

private:
  bool Invalidate(std::string reason);

  bool m_valid = true;

  std::string m_path;
  std::string m_error;

  std::shared_ptr<Manifest> m_manifest;
  std::vector<std::string> m_textures;
  std::vector<char> m_logo_data;
};
}  // namespace ResourcePack
