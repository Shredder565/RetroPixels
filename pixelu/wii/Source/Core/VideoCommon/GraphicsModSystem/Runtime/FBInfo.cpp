// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Runtime/FBInfo.h"

#include <type_traits>

#include "Common/Hash.h"

static_assert(std::has_unique_object_representations_v<FBInfo>,
              "hashing the raw bytes needs a struct without padding");

u32 FBInfo::CalculateHash() const
{
  return Common::HashAdler32(reinterpret_cast<const u8*>(this), sizeof(*this));
}

bool FBInfo::operator==(const FBInfo& other) const
{
  return m_width == other.m_width && m_height == other.m_height &&
         m_texture_format == other.m_texture_format;
}
