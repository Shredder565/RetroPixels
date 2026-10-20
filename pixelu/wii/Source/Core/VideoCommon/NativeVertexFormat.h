// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <type_traits>

#include "Common/CommonTypes.h"
#include "VideoCommon/CPMemory.h"

// Bits of a loader's "native components" mask: which host vertex attributes a vertex carries.
// The position is always there and has no bit.
enum : u32
{
  // One bit per texture matrix index byte, in texture coordinate order.
  VB_HAS_TEXMTXIDX0 = 1u << 0,
  VB_HAS_TEXMTXIDX1 = 1u << 1,
  VB_HAS_TEXMTXIDX2 = 1u << 2,
  VB_HAS_TEXMTXIDX3 = 1u << 3,
  VB_HAS_TEXMTXIDX4 = 1u << 4,
  VB_HAS_TEXMTXIDX5 = 1u << 5,
  VB_HAS_TEXMTXIDX6 = 1u << 6,
  VB_HAS_TEXMTXIDX7 = 1u << 7,
  VB_HAS_TEXMTXIDXALL = 0xffu << 0,

  VB_HAS_POSMTXIDX = 1u << 8,

  VB_HAS_NORMAL = 1u << 9,
  VB_HAS_TANGENT = 1u << 10,
  VB_HAS_BINORMAL = 1u << 11,

  VB_COL_SHIFT = 12,
  VB_HAS_COL0 = 1u << 12,
  VB_HAS_COL1 = 1u << 13,
  // Position matrix index, normal/tangent/binormal and both colours.
  VB_HAS_SHARED = 0x3fu << 8,

  VB_HAS_UV0 = 1u << 16,
  VB_HAS_UV1 = 1u << 17,
  VB_HAS_UV2 = 1u << 18,
  VB_HAS_UV3 = 1u << 19,
  VB_HAS_UV4 = 1u << 20,
  VB_HAS_UV5 = 1u << 21,
  VB_HAS_UV6 = 1u << 22,
  VB_HAS_UV7 = 1u << 23,
  VB_HAS_UVALL = 0xffu << 16,
  VB_HAS_UVTEXMTXSHIFT = 16,
};

// Where one attribute sits in a host vertex and how the GPU should read it.
struct AttributeFormat
{
  ComponentFormat type;
  int components;
  int offset;
  bool enable;
  bool integer;
};

// The host vertex layout a vertex loader produces. Declarations are compared and hashed as raw
// bytes, so every instance starts out all-zero, padding included.
struct PortableVertexDeclaration
{
  int stride;

  AttributeFormat position;
  std::array<AttributeFormat, 3> normals;
  std::array<AttributeFormat, 2> colors;
  std::array<AttributeFormat, 8> texcoords;
  AttributeFormat posmtx;

  PortableVertexDeclaration() { std::memset(static_cast<void*>(this), 0, sizeof(*this)); }

  bool operator<(const PortableVertexDeclaration& other) const
  {
    return std::memcmp(this, &other, sizeof(*this)) < 0;
  }
  bool operator==(const PortableVertexDeclaration& other) const
  {
    return std::memcmp(this, &other, sizeof(*this)) == 0;
  }
};

static_assert(std::is_trivially_copyable_v<PortableVertexDeclaration>,
              "declarations are zeroed and compared with memset/memcmp");
static_assert(sizeof(PortableVertexDeclaration) % sizeof(u32) == 0);

template <>
struct std::hash<PortableVertexDeclaration>
{
  size_t operator()(const PortableVertexDeclaration& decl) const noexcept
  {
    // Mix the declaration a 32-bit word at a time (multiply-xorshift).
    constexpr size_t word_count = sizeof(decl) / sizeof(u32);
    std::array<u32, word_count> words;
    std::memcpy(words.data(), &decl, sizeof(decl));

    u64 acc = 0x51ed27d3a8b4f6c1ULL;
    for (const u32 word : words)
    {
      acc = (acc ^ word) * 0x9e3779b97f4a7c15ULL;
      acc ^= acc >> 29;
    }
    return static_cast<size_t>(acc);
  }
};

// A backend's vertex input layout object for one declaration. Backends derive from this; the
// loaders only ever produce data matching the declaration they were built for.
class NativeVertexFormat
{
public:
  explicit NativeVertexFormat(const PortableVertexDeclaration& vtx_decl) : m_decl(vtx_decl) {}
  virtual ~NativeVertexFormat() = default;

  NativeVertexFormat(const NativeVertexFormat&) = delete;
  NativeVertexFormat& operator=(const NativeVertexFormat&) = delete;
  NativeVertexFormat(NativeVertexFormat&&) = default;
  NativeVertexFormat& operator=(NativeVertexFormat&&) = default;

  u32 GetVertexStride() const { return static_cast<u32>(m_decl.stride); }
  const PortableVertexDeclaration& GetVertexDeclaration() const { return m_decl; }

protected:
  PortableVertexDeclaration m_decl;
};
