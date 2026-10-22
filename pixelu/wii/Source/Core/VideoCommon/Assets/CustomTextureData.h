// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <span>
#include <string>
#include <vector>

#include "Common/Buffer.h"
#include "Common/CommonTypes.h"
#include "VideoCommon/TextureConfig.h"

namespace VideoCommon
{
// Decoded custom texture: one or more array slices (six for a cube map), each with its mip
// chain from largest to smallest.
class CustomTextureData
{
public:
  struct ArraySlice
  {
    struct Level
    {
      Common::UniqueBuffer<u8> data;
      AbstractTextureFormat format = AbstractTextureFormat::RGBA8;
      u32 width = 0;
      u32 height = 0;
      // Texels per row in `data` (may exceed width when rows are padded).
      u32 row_length = 0;
    };
    std::vector<Level> m_levels;
  };
  std::vector<ArraySlice> m_slices;
};

// Appends every slice and mip level stored in a DDS file.
bool LoadDDSTexture(CustomTextureData* texture, const std::string& filename);
// Reads the top level of a DDS file into `level`; mip_level is the level it will be used as.
bool LoadDDSTexture(CustomTextureData::ArraySlice::Level* level, const std::string& filename,
                    u32 mip_level);
// Decodes a PNG (from a file or from memory) to RGBA8.
bool LoadPNGTexture(CustomTextureData::ArraySlice::Level* level, const std::string& filename);
bool LoadPNGTexture(CustomTextureData::ArraySlice::Level* level, std::span<const u8> buffer);
}  // namespace VideoCommon
