// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// PolyHeader.h
//
// Reads the 7-word header that starts every polygon of a Real3D model.
// After the header come the polygon's new vertices, 4 words each (X, Y, Z
// with the normal or shade in their low bytes, then U/V); vertices shared
// with the previous polygon are not stored again.
//
// Word 0  bits 31-26 specular coefficient    bit 25 clockwise winding
//         bits 9-8 discard flags             bit 7 specular enable
//         bit 6 quad (else triangle)         bits 3-0 vertex N shared
// Word 1  bits 31-8 normal X (2.22)          bit 7 edge-on translucency
//         bit 6 UV scale (1: 16.0, 0: 13.3)  bit 5 fixed shading
//         bit 4 double sided                 bit 3 smooth shading
//         bit 2 last polygon                 bit 1 RGB colour (else table)
//         bit 0 no line-of-sight return
// Word 2  bits 31-8 normal Y                 bits 7-5 microtexture number
//         bit 4 microtexture                 bits 3-2 microtexture min LOD
//         bit 1 U mirror                     bit 0 V mirror
// Word 3  bits 31-8 normal Z                 bit 7 U smooth, bit 6 V smooth
//         bits 5-3 width, 2-0 height (32 << n texels)
// Word 4  bits 31-8 RGB colour or colour table index (bits 19-8)
//         bit 7 translator map               bit 6 texture page
//         bits 4-0 texture X bits 6-2 (in 32-texel units)
// Word 5  bits 31-8 texture NP               bit 7 texture X bit 1
//         bits 4-0 texture Y (32-texel units)
// Word 6  bit 31 contour (alpha test)        bits 30-24 translator map offset
//         bit 23 opaque                      bits 22-18 transparency (0-32)
//         bit 17 translucency layer select   bit 16 luminous (no lighting)
//         bits 15-11 light modifier          bit 10 texture enable
//         bits 9-7 texture format            bits 6-5 shininess
//         bit 4 high priority                bit 3 layered
//         bits 2-0 texture alpha mode

#ifndef INCLUDED_NEW3D_POLYHEADER_H
#define INCLUDED_NEW3D_POLYHEADER_H

#include <cstdint>

namespace New3D {

class PolyHeader
{
public:
  PolyHeader() = default;
  explicit PolyHeader(const uint32_t *words) : header(words) {}
  PolyHeader &operator=(const uint32_t *words) { header = words; return *this; }

  // Walking a model
  const uint32_t *StartOfData() const { return header + 7; }
  bool NextPoly();                  // false at the last polygon
  int NumTrianglesTotal() const;    // whole model from here, quads counting 2

  // Word 0
  bool SpecularEnabled() const { return Bit(0, 7); }
  float SpecularValue() const { return float(header[0] >> 26) / 63.f; }
  bool Discard() const { return (header[0] & 0x300) == 0x300; }
  bool Discard1() const { return Bit(0, 9); }
  bool Discard2() const { return Bit(0, 8); }
  int NumVerts() const { return Bit(0, 6) ? 4 : 3; }
  int NumSharedVerts() const;
  bool SharedVertex(int n) const { return Bit(0, unsigned(n)); }

  // Word 1
  void FaceNormal(float n[3]) const;
  float UVScale() const { return Bit(1, 6) ? 1.0f : (1.0f / 8.0f); }
  bool DoubleSided() const { return Bit(1, 4); }
  bool LastPoly() const { return Bit(1, 2); }
  bool PolyColor() const { return Bit(1, 1); }
  bool FixedShading() const { return Bit(1, 5); }
  bool SmoothShading() const { return Bit(1, 3); }
  bool NoLosReturn() const { return Bit(1, 0); }

  // Word 2
  bool TexUMirror() const { return Bit(2, 1); }
  bool TexVMirror() const { return Bit(2, 0); }
  bool MicroTexture() const { return Bit(2, 4); }
  int MicroTextureID() const { return int((header[2] >> 5) & 7); }
  int MicroTextureMinLOD() const { return int((header[2] >> 2) & 3); }

  // Word 3
  int TexWidth() const { return TexSize(header[3] >> 3); }
  int TexHeight() const { return TexSize(header[3]); }
  bool TexSmoothU() const { return Bit(3, 7); }
  bool TexSmoothV() const { return Bit(3, 6); }

  // Word 4
  int ColorIndex() const { return int((header[4] >> 8) & 0xFFF); }
  bool TranslatorMap() const { return Bit(4, 7); }
  int Page() const { return Bit(4, 6) ? 1 : 0; }
  int X() const;
  int Y() const { return int((32 * (header[5] & 0x1F)) & 2047); }

  // Word 5
  float TextureNP() const { return float(header[5] >> 8); }

  // Word 6
  bool AlphaTest() const { return Bit(6, 31); }
  int TranslatorMapOffset() const { return int((header[6] >> 24) & 0x7F); }
  uint8_t Transparency() const;     // 0-255
  bool PolyAlpha() const { return !Bit(6, 23); }
  bool TranslucencyPatternSelect() const { return Bit(6, 17); }
  bool LightEnabled() const { return !Bit(6, 16); }
  float LightModifier() const { return float((header[6] >> 11) & 0x1F) * (1.0f / 16.0f); }
  bool TexEnabled() const { return Bit(6, 10); }
  int TexFormat() const { return int((header[6] >> 7) & 7); }
  float Shininess() const { return float((header[6] >> 5) & 3); }
  bool HighPriority() const { return Bit(6, 4); }
  bool Layered() const { return Bit(6, 3); }
  bool TextureAlpha() const { return (header[6] & 7) != 0; }

  // A key made of every header field that affects how a polygon is drawn;
  // polygons with equal keys are drawn together.
  uint64_t Hash() const;

  const uint32_t *header = nullptr;

private:
  bool Bit(int word, unsigned bit) const { return ((header[word] >> bit) & 1) != 0; }
  static int TexSize(uint32_t field)
  {
    uint32_t n = field & 7;
    return 32 << (n >= 6 ? 0 : n);   // 6 and 7 are not valid sizes
  }
};

} // New3D

#endif  // INCLUDED_NEW3D_POLYHEADER_H
