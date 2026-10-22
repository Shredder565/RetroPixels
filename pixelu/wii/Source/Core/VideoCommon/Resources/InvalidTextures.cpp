// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Resources/InvalidTextures.h"

#include <array>

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/TextureConfig.h"

namespace VideoCommon
{
namespace
{
using Pixel = std::array<u8, 4>;

constexpr Pixel CLEAR{0, 0, 0, 0};
constexpr Pixel MAGENTA{255, 0, 255, 255};

// Creates a 1x1 RGBA8 texture with `layers` layers, each filled with `pixel`.
std::unique_ptr<AbstractTexture> SolidTexture(AbstractTextureType type, u32 layers,
                                              const Pixel& pixel, std::string_view name)
{
  const TextureConfig config(1, 1, 1, layers, 1, AbstractTextureFormat::RGBA8, 0, type);
  std::unique_ptr<AbstractTexture> texture = g_gfx->CreateTexture(config, name);
  for (u32 layer = 0; layer < layers; ++layer)
    texture->Load(0, 1, 1, 1, pixel.data(), pixel.size(), layer);
  return texture;
}
}  // namespace

std::unique_ptr<AbstractTexture> CreateInvalidTransparentTexture()
{
  return SolidTexture(AbstractTextureType::Texture_2D, 1, CLEAR, "Invalid Transparent Texture");
}

std::unique_ptr<AbstractTexture> CreateInvalidColorTexture()
{
  return SolidTexture(AbstractTextureType::Texture_2D, 1, MAGENTA, "Invalid Color Texture");
}

std::unique_ptr<AbstractTexture> CreateInvalidCubemapTexture()
{
#ifdef __APPLE__
  // Cube maps misbehave on Metal; callers cope with a missing texture.
  return nullptr;
#else
  return SolidTexture(AbstractTextureType::Texture_CubeMap, 6, MAGENTA, "Invalid Cubemap Texture");
#endif
}

std::unique_ptr<AbstractTexture> CreateInvalidArrayTexture()
{
  return SolidTexture(AbstractTextureType::Texture_2DArray, 1, MAGENTA, "Invalid Array Texture");
}
}  // namespace VideoCommon
