// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace DiscIO
{
// The banner.bin of a title's save data on the NAND: a "WIBN" header with the save's name and
// description, a 192x64 banner image and one or more 48x48 icons (all RGB5A3).
class WiiSaveBanner
{
public:
  static constexpr u32 BANNER_WIDTH = 192;
  static constexpr u32 BANNER_HEIGHT = 64;

  // Looks in the configured NAND first, then in the banner cache.
  explicit WiiSaveBanner(u64 title_id);

  bool IsValid() const { return m_valid; }
  const std::string& GetPath() const { return m_path; }
  std::string GetName() const;
  std::string GetDescription() const;

  std::vector<u32> GetBanner(u32* width, u32* height) const;

private:
  // Stored big-endian; only the UTF-16 strings are used.
  struct Header
  {
    char magic[4];
    u32 flags;
    u16 animation_speed;
    u8 unused[22];
    char16_t name[32];
    char16_t description[32];
  } m_header;

  bool m_valid = true;
  std::string m_path;
};
}  // namespace DiscIO
