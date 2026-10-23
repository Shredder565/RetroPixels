// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// TextureRefs.h
//
// The set of distinct texture rectangles a cached model uses, so that they
// can all be (re)decoded each time the model is drawn.

#ifndef INCLUDED_TEXTUREREFS_H
#define INCLUDED_TEXTUREREFS_H

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace Legacy3D {

class CLegacy3D;

class CTextureRefs
{
public:
  // Number of distinct rectangles held.
  unsigned GetSize() const { return unsigned(m_order.size()); }

  void Clear();

  // A rectangle is format (0-7), position and size in texels. Positions
  // and sizes are kept to whole 32-texel tiles below 2048 (a size of 2048
  // or more is kept as its remainder), which is how they are given back.
  bool ContainsRef(unsigned fmt, unsigned x, unsigned y, unsigned width, unsigned height) const;
  bool AddRef(unsigned fmt, unsigned x, unsigned y, unsigned width, unsigned height);   // always true
  bool RemoveRef(unsigned fmt, unsigned x, unsigned y, unsigned width, unsigned height);

  // Asks the renderer to decode every rectangle, in the order they were
  // first added.
  void DecodeAllTextures(CLegacy3D *renderer) const;

private:
  static uint32_t Key(unsigned fmt, unsigned x, unsigned y, unsigned width, unsigned height);

  std::vector<uint32_t> m_order;
  std::unordered_set<uint32_t> m_present;
};

} // Legacy3D

#endif  // INCLUDED_TEXTUREREFS_H
