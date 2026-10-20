// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/IndexGenerator.h"

#include <cstring>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
// The index that ends a strip when primitive restart is on. It is never a vertex index.
constexpr u16 RESTART = 0xffff;

// Appends indices; vertex numbers are relative to `first` and wrap to 16 bits. The writer is
// passed by value and handed back, so its pointer stays in a register in the loops.
struct IndexWriter
{
  u16* out;
  u32 first;

  void Vertex(u32 n) { *out++ = static_cast<u16>(first + n); }
  void Raw(u32 value) { *out++ = static_cast<u16>(value); }
  void Restart() { *out++ = RESTART; }

  // One triangle; as a strip of its own when restart is on.
  template <bool UseRestart>
  void Triangle(u32 a, u32 b, u32 c)
  {
    Vertex(a);
    Vertex(b);
    Vertex(c);
    if constexpr (UseRestart)
      Restart();
  }
};

template <bool UseRestart>
IndexWriter Triangles(IndexWriter w, u32 n)
{
  for (u32 v = 0; v + 3 <= n; v += 3)
    w.Triangle<UseRestart>(v, v + 1, v + 2);
  return w;
}

template <bool UseRestart>
IndexWriter TriangleStrip(IndexWriter w, u32 n)
{
  if constexpr (UseRestart)
  {
    // The strip as it is, then a restart (even when it has fewer than three vertices).
    for (u32 v = 0; v < n; v++)
      w.Vertex(v);
    w.Restart();
  }
  else
  {
    // Triangles in pairs; the second of each pair swaps its last two vertices to keep the
    // winding.
    u32 v = 0;
    for (; v + 3 < n; v += 2)
    {
      w.Triangle<false>(v, v + 1, v + 2);
      w.Triangle<false>(v + 1, v + 3, v + 2);
    }
    if (v + 2 < n)
      w.Triangle<false>(v, v + 1, v + 2);
  }
  return w;
}

// A fan around vertex 0. With restart, three fan triangles (0,k-1,k), (0,k,k+1), (0,k+1,k+2)
// can be sent as the strip k-1, k, 0, k+1, k+2, and two as k-1, k, 0, k+1.
template <bool UseRestart>
IndexWriter TriangleFan(IndexWriter w, u32 n)
{
  u32 k = 2;
  if constexpr (UseRestart)
  {
    for (; k + 3 <= n; k += 3)
    {
      w.Vertex(k - 1);
      w.Vertex(k);
      w.Vertex(0);
      w.Vertex(k + 1);
      w.Vertex(k + 2);
      w.Restart();
    }
    for (; k + 2 <= n; k += 2)
    {
      w.Vertex(k - 1);
      w.Vertex(k);
      w.Vertex(0);
      w.Vertex(k + 1);
      w.Restart();
    }
  }
  for (; k < n; k++)
    w.Triangle<UseRestart>(0, k - 1, k);
  return w;
}

// Quads a, b, c, d become (a, b, c) and (a, c, d), or the strip b, c, a, d. Three vertices
// left over at the end still make a triangle (Wind Waker's sun rays rely on this).
template <bool UseRestart>
IndexWriter Quads(IndexWriter w, u32 n)
{
  u32 q = 0;
  for (; q + 4 <= n; q += 4)
  {
    if constexpr (UseRestart)
    {
      w.Vertex(q + 1);
      w.Vertex(q + 2);
      w.Vertex(q);
      w.Vertex(q + 3);
      w.Restart();
    }
    else
    {
      w.Triangle<false>(q, q + 1, q + 2);
      w.Triangle<false>(q, q + 2, q + 3);
    }
  }
  if (n - q == 3)
    w.Triangle<UseRestart>(q, q + 1, q + 2);
  return w;
}

// Line segments: pairs of vertices, or each consecutive pair of a strip.
IndexWriter Lines(IndexWriter w, u32 n)
{
  for (u32 v = 0; v + 1 < n; v += 2)
  {
    w.Vertex(v);
    w.Vertex(v + 1);
  }
  return w;
}

IndexWriter LineStrip(IndexWriter w, u32 n)
{
  for (u32 v = 0; v + 1 < n; v++)
  {
    w.Vertex(v);
    w.Vertex(v + 1);
  }
  return w;
}

IndexWriter Points(IndexWriter w, u32 n)
{
  for (u32 v = 0; v < n; v++)
    w.Vertex(v);
  return w;
}

// Vertex shader expansion: host vertex 4*v + corner stands for corner 0..3 of GX vertex v.
// A line uses corners 0 and 1 of its first vertex and 2 and 3 of its second, drawn as a
// two-triangle strip or as two triangles.
template <bool UseRestart>
IndexWriter ExpandedLines(IndexWriter w, u32 n, u32 step)
{
  for (u32 v = 0; v + 1 < n; v += step)
  {
    const u32 a = (w.first + v) << 2;
    const u32 b = (w.first + v + 1) << 2;
    if constexpr (UseRestart)
    {
      w.Raw(a);
      w.Raw(a + 1);
      w.Raw(b + 2);
      w.Raw(b + 3);
      w.Restart();
    }
    else
    {
      w.Raw(a);
      w.Raw(a + 1);
      w.Raw(b + 2);
      w.Raw(a + 1);
      w.Raw(b + 2);
      w.Raw(b + 3);
    }
  }
  return w;
}

template <bool UseRestart>
IndexWriter ExpandedPoints(IndexWriter w, u32 n)
{
  for (u32 v = 0; v < n; v++)
  {
    const u32 corner0 = (w.first + v) << 2;
    if constexpr (UseRestart)
    {
      for (u32 c = 0; c < 4; c++)
        w.Raw(corner0 + c);
      w.Restart();
    }
    else
    {
      w.Raw(corner0);
      w.Raw(corner0 + 1);
      w.Raw(corner0 + 2);
      w.Raw(corner0 + 1);
      w.Raw(corner0 + 2);
      w.Raw(corner0 + 3);
    }
  }
  return w;
}

template <bool UseRestart>
IndexWriter Generate(OpcodeDecoder::Primitive primitive, bool vs_expand, IndexWriter w, u32 n)
{
  using OpcodeDecoder::Primitive;
  switch (primitive)
  {
  case Primitive::GX_DRAW_QUADS_2:
    WARN_LOG_FMT(VIDEO, "Non-standard primitive drawing command GL_DRAW_QUADS_2");
    return Quads<UseRestart>(w, n);
  case Primitive::GX_DRAW_QUADS:
    return Quads<UseRestart>(w, n);
  case Primitive::GX_DRAW_TRIANGLES:
    return Triangles<UseRestart>(w, n);
  case Primitive::GX_DRAW_TRIANGLE_STRIP:
    return TriangleStrip<UseRestart>(w, n);
  case Primitive::GX_DRAW_TRIANGLE_FAN:
    return TriangleFan<UseRestart>(w, n);
  case Primitive::GX_DRAW_LINES:
    return vs_expand ? ExpandedLines<UseRestart>(w, n, 2) : Lines(w, n);
  case Primitive::GX_DRAW_LINE_STRIP:
    return vs_expand ? ExpandedLines<UseRestart>(w, n, 1) : LineStrip(w, n);
  case Primitive::GX_DRAW_POINTS:
    return vs_expand ? ExpandedPoints<UseRestart>(w, n) : Points(w, n);
  }
  return w;
}
}  // namespace

void IndexGenerator::Init()
{
  m_restart = g_backend_info.bSupportsPrimitiveRestart;
  m_vs_expand = g_Config.UseVSForLinePointExpand();
}

void IndexGenerator::Start(u16* index_ptr)
{
  m_cursor = index_ptr;
  m_start = index_ptr;
  m_next_vertex = 0;
}

void IndexGenerator::AddIndices(OpcodeDecoder::Primitive primitive, u32 num_vertices)
{
  const IndexWriter w{m_cursor, m_next_vertex};
  m_cursor = m_restart ? Generate<true>(primitive, m_vs_expand, w, num_vertices).out :
                         Generate<false>(primitive, m_vs_expand, w, num_vertices).out;
  m_next_vertex += num_vertices;
}

void IndexGenerator::AddExternalIndices(const u16* indices, u32 num_indices, u32 num_vertices)
{
  std::memcpy(m_cursor, indices, sizeof(u16) * num_indices);
  m_cursor += num_indices;
  m_next_vertex += num_vertices;
}

u32 IndexGenerator::GetRemainingIndices(OpcodeDecoder::Primitive primitive) const
{
  // Index 0xffff is the restart index, but it never has to be a vertex: a game sends at most
  // 0xffff vertices in one draw, numbered 0..0xfffe from an empty batch (Pocoyo Racing sends
  // exactly that many; https://bugs.dolphin-emu.org/issues/13136). Expanded lines and points
  // use four host vertices per GX vertex.
  u32 limit = 0xffff;
  if (g_Config.UseVSForLinePointExpand() && primitive >= OpcodeDecoder::Primitive::GX_DRAW_LINES)
    limit >>= 2;

  if (m_next_vertex > limit) [[unlikely]]
  {
    PanicAlertFmt("GetRemainingIndices would overflow; we've already written too many indices? "
                  "base index {} > max index {}",
                  m_next_vertex, limit);
    return 0;
  }
  return limit - m_next_vertex;
}
