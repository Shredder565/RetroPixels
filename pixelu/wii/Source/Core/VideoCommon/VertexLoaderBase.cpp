// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/VertexLoaderBase.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <vector>

#include <fmt/ranges.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"

#include "VideoCommon/VertexLoader.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexLoader_Color.h"
#include "VideoCommon/VertexLoader_Normal.h"
#include "VideoCommon/VertexLoader_Position.h"
#include "VideoCommon/VertexLoader_TextCoord.h"
#include "VideoCommon/VertexLoaderX64.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
// Everything a loader leaves behind for z-freeze and the missing-normal workarounds.
struct LoaderCaches
{
  std::array<u32, 3> posmtx;
  std::array<std::array<float, 4>, 3> position;
  std::array<float, 4> normal;
  std::array<float, 4> tangent;
  std::array<float, 4> binormal;

  static LoaderCaches Capture()
  {
    return {VertexLoaderManager::position_matrix_index_cache, VertexLoaderManager::position_cache,
            VertexLoaderManager::normal_cache, VertexLoaderManager::tangent_cache,
            VertexLoaderManager::binormal_cache};
  }

  void Restore() const
  {
    VertexLoaderManager::position_matrix_index_cache = posmtx;
    VertexLoaderManager::position_cache = position;
    VertexLoaderManager::normal_cache = normal;
    VertexLoaderManager::tangent_cache = tangent;
    VertexLoaderManager::binormal_cache = binormal;
  }
};

// Compares floats by their bits, so NaNs (some games draw them) compare equal to themselves.
bool SameBits(const float* x, const float* y, size_t n)
{
  for (size_t i = 0; i < n; i++)
  {
    if (std::bit_cast<u32>(x[i]) != std::bit_cast<u32>(y[i]))
      return false;
  }
  return true;
}

// "Compare" mode: runs the C++ loader (the reference) and the native one on every batch and
// reports any difference. The reference's output is the one used.
class ComparingVertexLoader final : public VertexLoaderBase
{
public:
  ComparingVertexLoader(std::unique_ptr<VertexLoaderBase> reference,
                        std::unique_ptr<VertexLoaderBase> candidate, const TVtxDesc& vtx_desc,
                        const VAT& vtx_attr)
      : VertexLoaderBase(vtx_desc, vtx_attr), m_reference(std::move(reference)),
        m_candidate(std::move(candidate))
  {
    ASSERT(m_reference && m_candidate);
    const VertexLoaderBase& ref = *m_reference;
    const VertexLoaderBase& cand = *m_candidate;
    if (ref.m_vertex_size != cand.m_vertex_size ||
        ref.m_native_components != cand.m_native_components ||
        ref.m_native_vtx_decl.stride != cand.m_native_vtx_decl.stride)
    {
      PanicAlertFmt("Can't compare vertex loaders that expect different vertex formats!\n"
                    "a: m_vertex_size {}, m_native_components {:#010x}, stride {}\n"
                    "b: m_vertex_size {}, m_native_components {:#010x}, stride {}",
                    ref.m_vertex_size, ref.m_native_components, ref.m_native_vtx_decl.stride,
                    cand.m_vertex_size, cand.m_native_components, cand.m_native_vtx_decl.stride);
      return;
    }
    // Both come from the same VCD and VAT, so these always agree with ours.
    ASSERT(m_vertex_size == ref.m_vertex_size && m_native_components == ref.m_native_components);
    m_native_vtx_decl = ref.m_native_vtx_decl;
  }

  int RunVertices(const u8* src, u8* dst, int count) override
  {
    const size_t n = static_cast<size_t>(count);
    m_out_reference.resize(n * static_cast<size_t>(m_reference->m_native_vtx_decl.stride) + 4);
    m_out_candidate.resize(n * static_cast<size_t>(m_candidate->m_native_vtx_decl.stride) + 4);

    const LoaderCaches before = LoaderCaches::Capture();
    const int ref_count = m_reference->RunVertices(src, m_out_reference.data(), count);
    const LoaderCaches after_ref = LoaderCaches::Capture();
    before.Restore();
    const int cand_count = m_candidate->RunVertices(src, m_out_candidate.data(), count);
    const LoaderCaches after_cand = LoaderCaches::Capture();

    Check(ref_count, cand_count, after_ref, after_cand);

    std::memcpy(dst, m_out_reference.data(),
                static_cast<size_t>(ref_count) * static_cast<size_t>(m_native_vtx_decl.stride));
    m_numLoadedVertices += count;
    return ref_count;
  }

private:
  void Check(int ref_count, int cand_count, const LoaderCaches& a, const LoaderCaches& b) const
  {
    ASSERT_MSG(VIDEO, ref_count == cand_count,
               "The two vertex loaders have loaded a different amount of vertices (a: {}, b: {}).",
               ref_count, cand_count);

    const size_t compared = static_cast<size_t>(std::min(ref_count, cand_count)) *
                            static_cast<size_t>(m_native_vtx_decl.stride);
    ASSERT_MSG(VIDEO,
               std::memcmp(m_out_reference.data(), m_out_candidate.data(), compared) == 0,
               "The two vertex loaders have loaded different data.  Configuration:"
               "\nVertex desc:\n{}\n\nVertex attr:\n{}",
               m_VtxDesc, m_VtxAttr);

    ASSERT_MSG(VIDEO, a.posmtx == b.posmtx,
               "Expected matching position matrix caches after loading (a: {}; b: {})",
               fmt::join(a.posmtx, ", "), fmt::join(b.posmtx, ", "));

    // Lanes past the position's own components may hold anything (the JIT stores whole SIMD
    // registers), so only the real components are compared.
    const size_t pos_lanes = m_VtxAttr.g0.PosElements == CoordComponentCount::XYZ ? 3 : 2;
    bool positions_equal = true;
    for (size_t v = 0; v < 3; v++)
      positions_equal = positions_equal && SameBits(a.position[v].data(), b.position[v].data(), pos_lanes);
    ASSERT_MSG(VIDEO, positions_equal,
               "Expected matching position caches after loading (a: {} / {} / {}; b: {} / {} / {})",
               fmt::join(a.position[0], ", "), fmt::join(a.position[1], ", "),
               fmt::join(a.position[2], ", "), fmt::join(b.position[0], ", "),
               fmt::join(b.position[1], ", "), fmt::join(b.position[2], ", "));

    ASSERT_MSG(VIDEO, SameBits(a.normal.data(), b.normal.data(), 3),
               "Expected matching normal caches after loading (a: {}; b: {})",
               fmt::join(a.normal, ", "), fmt::join(b.normal, ", "));
    ASSERT_MSG(VIDEO, SameBits(a.tangent.data(), b.tangent.data(), 3),
               "Expected matching tangent caches after loading (a: {}; b: {})",
               fmt::join(a.tangent, ", "), fmt::join(b.tangent, ", "));
    ASSERT_MSG(VIDEO, SameBits(a.binormal.data(), b.binormal.data(), 3),
               "Expected matching binormal caches after loading (a: {}; b: {})",
               fmt::join(a.binormal, ", "), fmt::join(b.binormal, ", "));
  }

  std::unique_ptr<VertexLoaderBase> m_reference;
  std::unique_ptr<VertexLoaderBase> m_candidate;
  std::vector<u8> m_out_reference;
  std::vector<u8> m_out_candidate;
};
}  // namespace

u32 VertexLoaderBase::GetVertexSize(const TVtxDesc& vtx_desc, const VAT& vtx_attr)
{
  // One byte per matrix index (position/normal and the eight texture matrices).
  u32 bytes = static_cast<u32>(std::popcount(vtx_desc.low.Hex & 0x1ffu));

  bytes += VertexLoader_Position::GetSize(vtx_desc.low.Position, vtx_attr.g0.PosFormat,
                                          vtx_attr.g0.PosElements);
  bytes += VertexLoader_Normal::GetSize(vtx_desc.low.Normal, vtx_attr.g0.NormalFormat,
                                        vtx_attr.g0.NormalElements, vtx_attr.g0.NormalIndex3);
  for (u32 c = 0; c < 2; c++)
    bytes += VertexLoader_Color::GetSize(vtx_desc.low.Color[c], vtx_attr.GetColorFormat(c));
  for (u32 t = 0; t < 8; t++)
  {
    bytes += VertexLoader_TextCoord::GetSize(vtx_desc.high.TexCoord[t], vtx_attr.GetTexFormat(t),
                                             vtx_attr.GetTexElements(t));
  }
  return bytes;
}

u32 VertexLoaderBase::GetVertexComponents(const TVtxDesc& vtx_desc, const VAT& vtx_attr)
{
  // The low nine VCD bits are the matrix index flags: bit 0 the position matrix, bits 1..8 the
  // texture matrices.
  const u32 matrix_flags = vtx_desc.low.Hex & 0x1ffu;
  u32 mask = ((matrix_flags & 1u) ? VB_HAS_POSMTXIDX : 0u) | (matrix_flags >> 1);

  if (vtx_desc.low.Normal != VertexComponentFormat::NotPresent)
  {
    mask |= VB_HAS_NORMAL;
    if (vtx_attr.g0.NormalElements == NormalComponentCount::NTB)
      mask |= VB_HAS_TANGENT | VB_HAS_BINORMAL;
  }
  for (u32 c = 0; c < 2; c++)
  {
    if (vtx_desc.low.Color[c] != VertexComponentFormat::NotPresent)
      mask |= VB_HAS_COL0 << c;
  }
  for (u32 t = 0; t < 8; t++)
  {
    if (vtx_desc.high.TexCoord[t] != VertexComponentFormat::NotPresent)
      mask |= VB_HAS_UV0 << t;
  }
  return mask;
}

std::unique_ptr<VertexLoaderBase> VertexLoaderBase::CreateVertexLoader(const TVtxDesc& vtx_desc,
                                                                       const VAT& vtx_attr)
{
  const VertexLoaderType type = g_ActiveConfig.vertex_loader_type;
  if (type == VertexLoaderType::Software)
    return std::make_unique<VertexLoader>(vtx_desc, vtx_attr);

  // The native loader is built first, then the reference (their constructors may report
  // problems, and they do so in this order).
  auto native = std::make_unique<VertexLoaderX64>(vtx_desc, vtx_attr);
  if (type != VertexLoaderType::Compare)
    return native;
  auto reference = std::make_unique<VertexLoader>(vtx_desc, vtx_attr);
  return std::make_unique<ComparingVertexLoader>(std::move(reference), std::move(native),
                                                 vtx_desc, vtx_attr);
}
