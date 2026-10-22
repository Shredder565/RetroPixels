// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/CommonTypes.h"

// The texture environment: indirect texturing, texture lookups, the TEV colour and alpha
// combiners, then the per-pixel tests and effects that follow them (alpha test, z texture, fog,
// late depth test, bounding box) before the pixel goes to the blender.
//
// One instance lives in the rasterizer. Its registers persist from pixel to pixel the way the
// hardware's do: a stage that samples no texture sees the last texture colour, for example.
class Tev
{
public:
  // A texture coordinate in s17.7 fixed point, already wrapped to the hardware's 24 bits.
  struct TexCoord
  {
    s32 s = 0;
    s32 t = 0;
  };

  // Inputs for the next pixel, written by the rasterizer before each Draw().
  std::array<s32, 3> Position{};             // x, y, z
  std::array<std::array<u8, 4>, 2> Color{};  // rasterized colours, R G B A
  std::array<TexCoord, 8> Uv{};
  std::array<s32, 4> IndirectLod{};  // s28.4
  std::array<bool, 4> IndirectLinear{};
  std::array<s32, 16> TextureLod{};
  std::array<bool, 16> TextureLinear{};

  // Copies the four konstant colours from the pixel shader constants.
  void SetKonstColors();

  // Shades the pixel described by the inputs and sends it on to the EFB.
  void Draw();

private:
  // A TEV register value: 11-bit signed range per component.
  struct Rgba
  {
    s16 r = 0;
    s16 g = 0;
    s16 b = 0;
    s16 a = 0;
  };

  void ApplyIndirect(u32 stage, s32 s, s32 t);
  Rgba RasterizedColor(u32 channel_select, u32 swap_table) const;

  // The combiner inputs of one stage, per channel: a, b, c (8 bits) and d (11 bits signed).
  struct StageInputs
  {
    std::array<s32, 4> a, b, c, d;  // indexed by the CH_ constants
  };
  StageInputs GatherInputs(u32 stage) const;
  void CombineColor(u32 stage, const StageInputs& in);
  void CombineAlpha(u32 stage, const StageInputs& in);

  std::array<Rgba, 4> m_regs{};  // PREV, C0, C1, C2
  std::array<Rgba, 4> m_konst{};
  Rgba m_tex{};      // texture colour after the stage's swap table
  Rgba m_tex_raw{};  // texture colour before swapping, used by z textures
  Rgba m_ras{};
  Rgba m_stage_konst{};

  u8 m_bump_alpha = 0;
  std::array<std::array<u8, 4>, 4> m_indirect_texel{};  // per indirect stage, R G B A
  TexCoord m_coord{};                                   // coordinate after indirect offset
};
