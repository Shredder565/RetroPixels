// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/AbstractTexture.h"

#include <algorithm>
#include <type_traits>

#include <imgui.h>

#include "Common/Assert.h"
#include "Common/Image.h"
#include "Common/MsgHandler.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractStagingTexture.h"

static_assert(std::is_same_v<AbstractTexture::imgui_texture_id, ImTextureID>,
              "AbstractTexture::imgui_texture_id has to be ImGui's ImTextureID type");

namespace
{
// Storage size of one texel, or of one 4x4 block for the BCn formats. 0 = not a storable format.
u32 UnitBytes(AbstractTextureFormat format)
{
  using F = AbstractTextureFormat;
  switch (format)
  {
  case F::R16:
  case F::D16:
    return 2;
  case F::RGBA8:
  case F::BGRA8:
  case F::RGB10_A2:
  case F::R32F:
  case F::D32F:
  case F::D24_S8:
    return 4;
  case F::DXT1:
  case F::D32F_S8:
  case F::RGBA16F:
    return 8;
  case F::DXT3:
  case F::DXT5:
  case F::BPTC:
    return 16;
  default:
    return 0;
  }
}
}  // namespace

AbstractTexture::AbstractTexture(const TextureConfig& c) : m_config(c)
{
}

AbstractTexture::operator ImTextureRef() const
{
  return ImTextureRef(static_cast<imgui_texture_id>(*this));
}

void AbstractTexture::FinishedRendering()
{
}

bool AbstractTexture::Save(const std::string& filename, unsigned int level, int compression) const
{
  // Only plain colour formats can go through an RGBA8 readback; compressed and HDR textures
  // would need a conversion pass first. Callers never ask for those.
  ASSERT(!IsCompressedFormat(m_config.format));
  ASSERT(level < m_config.levels);
  ASSERT(m_config.format != AbstractTextureFormat::RGBA16F);

  const u32 width = std::max(1u, m_config.width >> level);
  const u32 height = std::max(1u, m_config.height >> level);

  // A one-off staging texture is fine here; saving is rare.
  const TextureConfig staging_config(width, height, 1, 1, 1, AbstractTextureFormat::RGBA8, 0,
                                     AbstractTextureType::Texture_2DArray);
  std::unique_ptr<AbstractStagingTexture> staging =
      g_gfx->CreateStagingTexture(StagingTextureType::Readback, staging_config);
  if (staging == nullptr)
    return false;

  staging->CopyFromTexture(this, 0, level);
  staging->Flush();
  if (!staging->Map())
    return false;

  const u8* pixels = reinterpret_cast<const u8*>(staging->GetMappedPointer());
  const u32 stride = static_cast<u32>(staging->GetMappedStride());
  return Common::SavePNG(filename, pixels, Common::ImageByteFormat::RGBA, width, height, stride,
                         compression);
}

bool AbstractTexture::IsCompressedFormat(AbstractTextureFormat format)
{
  return GetBlockSizeForFormat(format) != 1;
}

bool AbstractTexture::IsDepthFormat(AbstractTextureFormat format)
{
  using F = AbstractTextureFormat;
  return format == F::D16 || format == F::D24_S8 || format == F::D32F || format == F::D32F_S8;
}

bool AbstractTexture::IsStencilFormat(AbstractTextureFormat format)
{
  using F = AbstractTextureFormat;
  return format == F::D24_S8 || format == F::D32F_S8;
}

bool AbstractTexture::IsCompatibleDepthAndColorFormats(AbstractTextureFormat depth_format,
                                                       AbstractTextureFormat color_format)
{
  using F = AbstractTextureFormat;
  // Depth formats that can be copied to and from a single-channel colour format of equal size.
  if (depth_format == F::D16)
    return color_format == F::R16;
  if (depth_format == F::D32F)
    return color_format == F::R32F;
  return false;
}

u32 AbstractTexture::CalculateStrideForFormat(AbstractTextureFormat format, u32 row_length)
{
  const u32 unit = UnitBytes(format);
  if (unit == 0)
  {
    PanicAlertFmt("Unhandled texture format.");
    return 0;
  }
  // Compressed rows hold at least one block.
  const u32 units_per_row = IsCompressedFormat(format) ? std::max(1u, row_length / 4) : row_length;
  return units_per_row * unit;
}

u32 AbstractTexture::GetTexelSizeForFormat(AbstractTextureFormat format)
{
  const u32 unit = UnitBytes(format);
  if (unit == 0)
    PanicAlertFmt("Unhandled texture format.");
  return unit;
}

u32 AbstractTexture::GetBlockSizeForFormat(AbstractTextureFormat format)
{
  using F = AbstractTextureFormat;
  const bool block_compressed =
      format == F::DXT1 || format == F::DXT3 || format == F::DXT5 || format == F::BPTC;
  return block_compressed ? 4 : 1;
}

const TextureConfig& AbstractTexture::GetConfig() const
{
  return m_config;
}
