// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// TileGenBuffer.h
//
// One 2D layer image produced by the tile generator: 384 lines of 512 ABGR
// pixels (only the first 496 of each line are visible). Zero is transparent.

#ifndef INCLUDED_TILEGENBUFFER_H
#define INCLUDED_TILEGENBUFFER_H

#include "Types.h"
#include <cstring>

struct TileGenBuffer
{
  static constexpr int Width = 512;
  static constexpr int Height = 384;

  UINT32 data[Width * Height];

  TileGenBuffer() { Clear(); }

  UINT32 *GetLine(int number) { return &data[number * Width]; }
  void Clear() { std::memset(data, 0, sizeof(data)); }
};

#endif  // INCLUDED_TILEGENBUFFER_H
