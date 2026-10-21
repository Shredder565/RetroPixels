// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "VideoCommon/TextureDecoder.h"

// Describes a framebuffer copy (EFB or XFB) for matching against mod targets.
struct FBInfo
{
  u32 m_height = 0;
  u32 m_width = 0;
  TextureFormat m_texture_format = TextureFormat::I4;
  // Adler-32 of the struct's bytes.
  u32 CalculateHash() const;
  bool operator==(const FBInfo& other) const;
};

struct FBInfoHasher
{
  std::size_t operator()(const FBInfo& fb_info) const noexcept
  {
    return static_cast<std::size_t>(fb_info.CalculateHash());
  }
};
