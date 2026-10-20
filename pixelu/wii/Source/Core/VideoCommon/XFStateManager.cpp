// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/XFStateManager.h"

#include <algorithm>
#include <cstring>

#include "Common/ChunkFile.h"

#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/XFMemory.h"

namespace
{
constexpr std::array<int, 2> NOTHING_PENDING = {-1, -1};

// True if `address` falls inside the `length` words starting at `base`.
bool Inside(u32 address, u32 base, u32 length)
{
  return address >= base && address < base + length;
}

// A position/texture matrix selected by a 6-bit index occupies 12 words at index * 4.
bool HitsRowMatrix(u32 address, u32 index)
{
  return Inside(address, index * 4, 12);
}

// Grows `span` to include [first, last]; an empty span simply becomes that range.
void Merge(std::array<int, 2>& span, int first, int last)
{
  if (span[0] == -1)
  {
    span = {first, last};
    return;
  }
  span[0] = std::min(span[0], first);
  span[1] = std::max(span[1], last);
}
}  // namespace

void XFStateManager::Init()
{
  m_texmtx_dirty = {false, false};
  m_posnrm_dirty = false;
  m_projection_dirty = false;
  m_viewport_dirty = false;
  m_texinfo_dirty = false;
  m_lighting_dirty = false;
  m_material_dirty = BitSet32(0);
  m_light_span = NOTHING_PENDING;
  m_posmtx_span = NOTHING_PENDING;
  m_nrmmtx_span = NOTHING_PENDING;
  m_postmtx_span = NOTHING_PENDING;

  std::memset(static_cast<void*>(&xfmem), 0, sizeof(xfmem));
}

void XFStateManager::DoState(PointerWrap& p)
{
  // Savestate order: the four spans, then the flags.
  for (Span* span : {&m_posmtx_span, &m_nrmmtx_span, &m_postmtx_span, &m_light_span})
    p.DoArray(*span);

  p.Do(m_material_dirty);
  p.DoArray(m_texmtx_dirty);
  p.Do(m_posnrm_dirty);
  p.Do(m_projection_dirty);
  p.Do(m_viewport_dirty);
  p.Do(m_texinfo_dirty);
  p.Do(m_lighting_dirty);

  // The projection constants depend on settings that may differ from when the state was saved.
  if (p.IsReadMode())
    m_projection_dirty = true;
}

void XFStateManager::InvalidateXFRange(int start, int end)
{
  // The matrix flags only look at the first written word, compared against the matrices the
  // current CP matrix indices select.
  const u32 first = static_cast<u32>(start);
  const auto& index_a = g_main_cp_state.matrix_index_a;
  const auto& index_b = g_main_cp_state.matrix_index_b;

  const u32 posnrm = index_a.PosNormalMtxIdx;
  // The normal matrix paired with a position matrix: 3x3 words in the normal matrix block.
  const u32 normal_base = XFMEM_NORMALMATRICES + (posnrm & 31) * 3;
  if (HitsRowMatrix(first, posnrm) || Inside(first, normal_base, 9))
    m_posnrm_dirty = true;

  if (HitsRowMatrix(first, index_a.Tex0MtxIdx) || HitsRowMatrix(first, index_a.Tex1MtxIdx) ||
      HitsRowMatrix(first, index_a.Tex2MtxIdx) || HitsRowMatrix(first, index_a.Tex3MtxIdx))
  {
    m_texmtx_dirty[0] = true;
  }

  if (HitsRowMatrix(first, index_b.Tex4MtxIdx) || HitsRowMatrix(first, index_b.Tex5MtxIdx) ||
      HitsRowMatrix(first, index_b.Tex6MtxIdx) || HitsRowMatrix(first, index_b.Tex7MtxIdx))
  {
    m_texmtx_dirty[1] = true;
  }

  // Position matrices: absolute addresses, upper end clamped to the block.
  if (start < XFMEM_POSMATRICES_END)
  {
    const int last = std::min<int>(end, XFMEM_POSMATRICES_END);
    if (m_posmtx_span[0] == -1)
    {
      m_posmtx_span = {start, last};
    }
    else
    {
      m_posmtx_span[0] = std::min(m_posmtx_span[0], start);
      // The upper bound is compared before clamping.
      if (m_posmtx_span[1] < end)
        m_posmtx_span[1] = last;
    }
  }

  // The remaining blocks record offsets from the block's base. A write starting below the
  // block records offset 0 for normal matrices, but the block's absolute base for the post
  // matrices and lights (consumers rely on these exact values).
  const auto block_end_offset = [end](int base, int limit) {
    return (end < limit ? end : limit) - base;
  };

  if (start < XFMEM_NORMALMATRICES_END && end > XFMEM_NORMALMATRICES)
  {
    const int lo = start < XFMEM_NORMALMATRICES ? 0 : start - XFMEM_NORMALMATRICES;
    Merge(m_nrmmtx_span, lo, block_end_offset(XFMEM_NORMALMATRICES, XFMEM_NORMALMATRICES_END));
  }

  if (start < XFMEM_POSTMATRICES_END && end > XFMEM_POSTMATRICES)
  {
    const int lo = start < XFMEM_POSTMATRICES ? XFMEM_POSTMATRICES : start - XFMEM_POSTMATRICES;
    Merge(m_postmtx_span, lo, block_end_offset(XFMEM_POSTMATRICES, XFMEM_POSTMATRICES_END));
  }

  if (start < XFMEM_LIGHTS_END && end > XFMEM_LIGHTS)
  {
    const int lo = start < XFMEM_LIGHTS ? XFMEM_LIGHTS : start - XFMEM_LIGHTS;
    Merge(m_light_span, lo, block_end_offset(XFMEM_LIGHTS, XFMEM_LIGHTS_END));
  }
}

void XFStateManager::SetTexMatrixChangedA(u32 value)
{
  auto& reg = g_main_cp_state.matrix_index_a;
  if (reg.Hex == value)
    return;

  // Pending geometry was built with the old indices.
  g_vertex_manager->Flush();
  if (reg.PosNormalMtxIdx != (value & 0x3f))
    m_posnrm_dirty = true;
  m_texmtx_dirty[0] = true;
  reg.Hex = value;
}

void XFStateManager::ResetTexMatrixAChange()
{
  m_texmtx_dirty[0] = false;
}

void XFStateManager::SetTexMatrixChangedB(u32 value)
{
  auto& reg = g_main_cp_state.matrix_index_b;
  if (reg.Hex == value)
    return;

  g_vertex_manager->Flush();
  m_texmtx_dirty[1] = true;
  reg.Hex = value;
}

void XFStateManager::ResetTexMatrixBChange()
{
  m_texmtx_dirty[1] = false;
}

void XFStateManager::ResetPosNormalChange()
{
  m_posnrm_dirty = false;
}

void XFStateManager::SetProjectionChanged()
{
  m_projection_dirty = true;
}

void XFStateManager::ResetProjection()
{
  m_projection_dirty = false;
}

void XFStateManager::SetViewportChanged()
{
  m_viewport_dirty = true;
}

void XFStateManager::ResetViewportChange()
{
  m_viewport_dirty = false;
}

void XFStateManager::SetTexMatrixInfoChanged([[maybe_unused]] int index)
{
  // One flag for all texgen entries: their constants are uploaded together anyway.
  m_texinfo_dirty = true;
}

void XFStateManager::ResetTexMatrixInfoChange()
{
  m_texinfo_dirty = false;
}

void XFStateManager::SetLightingConfigChanged()
{
  m_lighting_dirty = true;
}

void XFStateManager::ResetLightingConfigChange()
{
  m_lighting_dirty = false;
}

void XFStateManager::ResetLightsChanged()
{
  m_light_span = NOTHING_PENDING;
}

void XFStateManager::SetMaterialColorChanged(int index)
{
  m_material_dirty[static_cast<size_t>(index)] = true;
}

void XFStateManager::ResetMaterialChanges()
{
  m_material_dirty = BitSet32(0);
}

void XFStateManager::ResetPerVertexTransformMatrixChanges()
{
  m_posmtx_span = NOTHING_PENDING;
}

void XFStateManager::ResetPerVertexNormalMatrixChanges()
{
  m_nrmmtx_span = NOTHING_PENDING;
}

void XFStateManager::ResetPostTransformMatrixChanges()
{
  m_postmtx_span = NOTHING_PENDING;
}
