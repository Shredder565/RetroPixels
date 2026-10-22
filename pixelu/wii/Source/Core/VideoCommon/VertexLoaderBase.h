// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>

#include "Common/CommonTypes.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/NativeVertexFormat.h"

// Identifies a vertex loader: the vertex descriptor (both halves) and one VAT entry, as raw
// register words. Two draws with the same key can share a loader.
class VertexLoaderUID
{
public:
  VertexLoaderUID() = default;
  VertexLoaderUID(const TVtxDesc& vtx_desc, const VAT& vat)
      : m_words{vtx_desc.low.Hex, vtx_desc.high.Hex, vat.g0.Hex, vat.g1.Hex, vat.g2.Hex}
  {
    u64 acc = 0;
    for (const u32 word : m_words)
      acc = ((acc << 7) | (acc >> 57)) ^ (word * 0x9e3779b1ULL);
    m_hash = static_cast<size_t>(acc);
  }

  bool operator==(const VertexLoaderUID& other) const { return m_words == other.m_words; }
  size_t GetHash() const { return m_hash; }

private:
  std::array<u32, 5> m_words{};
  size_t m_hash = 0;
};

template <>
struct std::hash<VertexLoaderUID>
{
  size_t operator()(const VertexLoaderUID& uid) const noexcept { return uid.GetHash(); }
};

// A vertex loader converts GX vertices (described by a VCD and a VAT entry) into the host
// layout in m_native_vtx_decl.
class VertexLoaderBase
{
public:
  // Bytes one GX vertex takes in the command stream.
  static u32 GetVertexSize(const TVtxDesc& vtx_desc, const VAT& vtx_attr);
  // The VB_HAS_* mask for a vertex of this format.
  static u32 GetVertexComponents(const TVtxDesc& vtx_desc, const VAT& vtx_attr);
  // Builds the loader the configuration asks for (C++, JIT, or both compared).
  static std::unique_ptr<VertexLoaderBase> CreateVertexLoader(const TVtxDesc& vtx_desc,
                                                              const VAT& vtx_attr);

  virtual ~VertexLoaderBase() = default;

  // Converts count vertices from src into dst. Returns how many vertices were written (indexed
  // positions with an all-ones index drop their vertex).
  virtual int RunVertices(const u8* src, u8* dst, int count) = 0;

  PortableVertexDeclaration m_native_vtx_decl{};
  const u32 m_vertex_size;
  const u32 m_native_components;

  // Set and read by VertexLoaderManager.
  NativeVertexFormat* m_native_vertex_format = nullptr;
  int m_numLoadedVertices = 0;

protected:
  VertexLoaderBase(const TVtxDesc& vtx_desc, const VAT& vtx_attr)
      : m_vertex_size{GetVertexSize(vtx_desc, vtx_attr)},
        m_native_components{GetVertexComponents(vtx_desc, vtx_attr)}, m_VtxAttr{vtx_attr},
        m_VtxDesc{vtx_desc}
  {
  }

  const VAT m_VtxAttr;
  const TVtxDesc m_VtxDesc;
};
