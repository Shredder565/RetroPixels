// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/CommonTypes.h"
#include "Common/Matrix.h"

// The software pipeline's two vertex records. InputVertexData is what the vertex loader decodes
// from the game's vertex stream; OutputVertexData is what the transform unit hands to primitive
// setup, the clipper and the rasterizer.

struct InputVertexData
{
  u8 posMtx;
  std::array<u8, 8> texMtx;

  Common::Vec3 position;
  // [0] is the normal, [1] the tangent (binormal T) and [2] the binormal (B).
  std::array<Common::Vec3, 3> normal;
  // Byte order A, B, G, R (index 0 is alpha).
  std::array<std::array<u8, 4>, 2> color;
  std::array<Common::Vec2, 8> texCoords;
};

struct OutputVertexData
{
  Common::Vec3 mvPosition = {};         // eye space position
  Common::Vec4 projectedPosition = {};  // clip space position
  Common::Vec3 screenPosition = {};     // window coordinates, filled in by the perspective divide
  std::array<Common::Vec3, 3> normal{};
  // Byte order R, G, B, A (index 0 is red).
  std::array<std::array<u8, 4>, 2> color{};
  std::array<Common::Vec3, 8> texCoords{};
};
