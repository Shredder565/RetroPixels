// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// TextureRefs.cpp
//
// Each rectangle is one 32-bit key: format in bits 24-26, then x, y, width
// and height in tile units (6 bits each, values taken modulo 2048).

#include "TextureRefs.h"
#include "Legacy3D.h"

#include <algorithm>

namespace Legacy3D {

static unsigned Tiles(unsigned texels)
{
  return (texels >> 5) & 0x3F;
}

uint32_t CTextureRefs::Key(unsigned fmt, unsigned x, unsigned y, unsigned width, unsigned height)
{
  return ((fmt & 7) << 24) | (Tiles(x) << 18) | (Tiles(y) << 12) | (Tiles(width) << 6) | Tiles(height);
}

void CTextureRefs::Clear()
{
  m_order.clear();
  m_present.clear();
}

bool CTextureRefs::ContainsRef(unsigned fmt, unsigned x, unsigned y, unsigned width, unsigned height) const
{
  return m_present.count(Key(fmt, x, y, width, height)) != 0;
}

bool CTextureRefs::AddRef(unsigned fmt, unsigned x, unsigned y, unsigned width, unsigned height)
{
  uint32_t key = Key(fmt, x, y, width, height);
  if (m_present.insert(key).second)
    m_order.push_back(key);
  return true;
}

bool CTextureRefs::RemoveRef(unsigned fmt, unsigned x, unsigned y, unsigned width, unsigned height)
{
  uint32_t key = Key(fmt, x, y, width, height);
  if (m_present.erase(key) == 0)
    return false;
  m_order.erase(std::find(m_order.begin(), m_order.end(), key));
  return true;
}

void CTextureRefs::DecodeAllTextures(CLegacy3D *renderer) const
{
  for (uint32_t key : m_order)
  {
    int fmt = int(key >> 24);
    int x = int((key >> 18) & 0x3F) << 5;
    int y = int((key >> 12) & 0x3F) << 5;
    int w = int((key >> 6) & 0x3F) << 5;
    int h = int(key & 0x3F) << 5;
    renderer->DecodeTexture(fmt, x, y, w, h);
  }
}

} // Legacy3D
