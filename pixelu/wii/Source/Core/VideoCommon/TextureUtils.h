// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <unordered_set>

#include "Common/CommonTypes.h"

class AbstractTexture;

// Writing game textures out as PNG files under the user's Dump/Textures/<game id> folder.
// File names are "<basename>[_arb][_mip<level>].png".
namespace VideoCommon::TextureUtils
{
// Remembers which names already exist in the dump folder (scanned recursively the first time
// it is used), so that each texture is written once without asking the file system each time.
class TextureDumper
{
public:
  void DumpTexture(const ::AbstractTexture& texture, std::string basename, u32 level,
                   bool is_arbitrary);

private:
  std::unordered_set<std::string> m_dumped_textures;
};

// One-off dump: writes the file unless it already exists.
void DumpTexture(const ::AbstractTexture& texture, std::string basename, u32 level,
                 bool is_arbitrary);

}  // namespace VideoCommon::TextureUtils
