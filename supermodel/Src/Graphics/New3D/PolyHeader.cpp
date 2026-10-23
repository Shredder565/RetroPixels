// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// PolyHeader.cpp

#include "PolyHeader.h"

namespace New3D {

int PolyHeader::NumSharedVerts() const
{
  uint32_t bits = header[0] & 0xF;
  int n = 0;
  for (; bits; bits >>= 1)
    n += int(bits & 1);
  return n;
}

bool PolyHeader::NextPoly()
{
  if (LastPoly())
    return false;
  header += 7 + 4 * (NumVerts() - NumSharedVerts());
  return true;
}

int PolyHeader::NumTrianglesTotal() const
{
  PolyHeader walk(header);
  int triangles = 0;
  do
    triangles += (walk.NumVerts() == 4) ? 2 : 1;
  while (walk.NextPoly());
  return triangles;
}

void PolyHeader::FaceNormal(float n[3]) const
{
  for (int i = 0; i < 3; i++)
    n[i] = float(int32_t(header[1 + i]) >> 8) * float(1.0 / 4194304.0);
}

int PolyHeader::X() const
{
  uint32_t tiles = ((header[4] & 0x1F) << 1) | ((header[5] >> 7) & 1);
  return int((32 * tiles) & 2047);
}

uint8_t PolyHeader::Transparency() const
{
  // Bit 23 means fully opaque; the 0-32 level beside it is then ignored
  // (some games leave garbage there).
  if (Bit(6, 23))
    return 255;
  return uint8_t((((header[6] >> 18) & 0x3F) * 255) / 32);
}

uint64_t PolyHeader::Hash() const
{
  uint64_t key = 0;
  key |= uint64_t(header[3] & 0xFF);                                      //  0-7  size, smoothing
  key |= uint64_t(((header[4] & 0x1F) << 1) | ((header[5] >> 7) & 1)) << 8; //  8-13 texture X
  key |= uint64_t(header[5] & 0x1F) << 14;                                // 14-18 texture Y
  key |= uint64_t((header[4] >> 6) & 3) << 19;                            // 19-20 page, translator map
  key |= uint64_t(DoubleSided()) << 21;
  key |= uint64_t(AlphaTest()) << 22;
  key |= uint64_t(PolyAlpha()) << 23;
  key |= uint64_t(header[2] & 0xFF) << 24;                                // 24-31 microtexture, mirroring
  key |= uint64_t(SpecularEnabled()) << 32;
  key |= uint64_t(SmoothShading()) << 33;
  key |= uint64_t(FixedShading()) << 34;
  key |= uint64_t(header[0] >> 26) << 35;                                 // 35-40 specular coefficient
  key |= uint64_t(header[6] & 0x3FFFF) << 41;                             // 41-58 word 6 low bits
  key |= uint64_t(NoLosReturn()) << 59;
  return key;
}

} // New3D
