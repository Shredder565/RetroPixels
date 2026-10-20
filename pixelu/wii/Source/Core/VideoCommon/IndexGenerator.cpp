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

// Appends indices; vertex numbers are relative to `first` and wrap to 16 bits.
struct IndexWriter
{
  u16* out;
  u32 first;

  void Vertex(u32 n) { *out++ = static_cast<u16>(first + n); }
  void Raw(u32 value) { *out++ = static_cast<u16>(value); }
  void Restart() { *out++ = RESTART; }

  // One triangle; as a strip of its own when restart is on.
  void Triangle(bool restart, u32 a, u32 b, u32 c)
  {
    Vertex(a);
    Vertex(b);
    Vertex(c);
    if (restart)
      Restart();
  }
};

void Triangles(IndexWriter& w, bool restart, u32 n)
{
  for (u32 v = 0; v + 3 <= n; v += 3)
    w.Triangle(restart, v, v + 1, v + 2);
}

void TriangleStrip(IndexWriter& w, bool restart, u32 n)
{
  if (restart)
  {
    // The strip as it is, then a restart (even when it has fewer than three vertices).
    for (u32 v = 0; v < n; v++)
      w.Vertex(v);
    w.Restart();
    return;
  }
  // Every other triangle swaps its last two vertices to keep the winding.
  for (u32 v = 0; v + 2 < n; v++)
  {
    if (v % 2 == 0)
      w.Triangle(false, v, v + 1, v + 2);
    else
      w.Triangle(false, v, v + 2, v + 1);
  }
}

// A fan around vertex 0. With restart, three fan triangles (0,k-1,k), (0,k,k+1), (0,k+1,k+2)
// can be sent as the strip k-1, k, 0, k+1, k+2, and two as k-1, k, 0, k+1.
void TriangleFan(IndexWriter& w, bool restart, u32 n)
{
  u32 k = 2;
  if (restart)
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
    w.Triangle(restart, 0, k - 1, k);
}

// Quads a, b, c, d become (a, b, c) and (a, c, d), or the strip b, c, a, d. Three vertices
// left over at the end still make a triangle (Wind Waker's sun rays rely on this).
void Quads(IndexWriter& w, bool restart, u32 n)
{
  u32 q = 0;
  for (; q + 4 <= n; q += 4)
  {
    if (restart)
    {
      w.Vertex(q + 1);
      w.Vertex(q + 2);
      w.Vertex(q);
      w.Vertex(q + 3);
      w.Restart();
    }
    else
    {
      w.Triangle(false, q, q + 1, q + 2);
      w.Triangle(false, q, q + 2, q + 3);
    }
  }
  if (n - q == 3)
    w.Triangle(restart, q, q + 1, q + 2);
}

// Line segments (pairs, or each consecutive pair of a strip) as host lines.
void Lines(IndexWriter& w, u32 n, bool strip)
{
  const u32 step = strip ? 1 : 2;
  for (u32 v = 0; v + 1 < n; v += step)
  {
    w.Vertex(v);
    w.Vertex(v + 1);
  }
}

// Vertex shader expansion: host vertex 4*v + corner stands for corner 0..3 of GX vertex v.
// A line uses corners 0 and 1 of its first vertex and 2 and 3 of its second, drawn as a
// two-triangle strip or as two triangles.
void ExpandedLines(IndexWriter& w, bool restart, u32 n, bool strip)
{
  const u32 step = strip ? 1 : 2;
  for (u32 v = 0; v + 1 < n; v += step)
  {
    const u32 a = (w.first + v) << 2;
    const u32 b = (w.first + v + 1) << 2;
    if (restart)
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
}

void ExpandedPoints(IndexWriter& w, bool restart, u32 n)
{
  for (u32 v = 0; v < n; v++)
  {
    const u32 corner0 = (w.first + v) << 2;
    if (restart)
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
  using OpcodeDecoder::Primitive;
  IndexWriter w{m_cursor, m_next_vertex};
  const u32 n = num_vertices;
  switch (primitive)
  {
  case Primitive::GX_DRAW_QUADS_2:
    WARN_LOG_FMT(VIDEO, "Non-standard primitive drawing command GL_DRAW_QUADS_2");
    Quads(w, m_restart, n);
    break;
  case Primitive::GX_DRAW_QUADS:
    Quads(w, m_restart, n);
    break;
  case Primitive::GX_DRAW_TRIANGLES:
    Triangles(w, m_restart, n);
    break;
  case Primitive::GX_DRAW_TRIANGLE_STRIP:
    TriangleStrip(w, m_restart, n);
    break;
  case Primitive::GX_DRAW_TRIANGLE_FAN:
    TriangleFan(w, m_restart, n);
    break;
  case Primitive::GX_DRAW_LINES:
  case Primitive::GX_DRAW_LINE_STRIP:
  {
    const bool strip = primitive == Primitive::GX_DRAW_LINE_STRIP;
    if (m_vs_expand)
      ExpandedLines(w, m_restart, n, strip);
    else
      Lines(w, n, strip);
    break;
  }
  case Primitive::GX_DRAW_POINTS:
    if (m_vs_expand)
    {
      ExpandedPoints(w, m_restart, n);
    }
    else
    {
      for (u32 v = 0; v < n; v++)
        w.Vertex(v);
    }
    break;
  }
  m_cursor = w.out;
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
