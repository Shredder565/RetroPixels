// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Software/Clipper.h"

#include <array>
#include <cmath>

#include "Common/CommonTypes.h"

#include "VideoBackends/Software/NativeVertexFormat.h"
#include "VideoBackends/Software/Rasterizer.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/XFMemory.h"

namespace Clipper
{
namespace
{
using Vertex = OutputVertexData;

// Outcode bits, one per side of the view volume.
enum : u32
{
  OUT_RIGHT = 1u << 0,
  OUT_LEFT = 1u << 1,
  OUT_TOP = 1u << 2,
  OUT_BOTTOM = 1u << 3,
  OUT_NEAR = 1u << 4,
  OUT_FAR = 1u << 5,
};

// A clip plane as the coefficients of x, y, z and w. A point is inside when the dot product
// with its clip-space position is not negative.
struct ClipPlane
{
  u32 outcode;
  float cx, cy, cz, cw;
};

// Triangles and lines use the same planes except for the near side: triangles test w alone,
// lines test w - z.
constexpr std::array<ClipPlane, 6> TRIANGLE_PLANES{{
    {OUT_RIGHT, -1.0f, 0.0f, 0.0f, 1.0f},
    {OUT_LEFT, 1.0f, 0.0f, 0.0f, 1.0f},
    {OUT_TOP, 0.0f, -1.0f, 0.0f, 1.0f},
    {OUT_BOTTOM, 0.0f, 1.0f, 0.0f, 1.0f},
    {OUT_NEAR, 0.0f, 0.0f, 0.0f, 1.0f},
    {OUT_FAR, 0.0f, 0.0f, 1.0f, 1.0f},
}};

constexpr std::array<ClipPlane, 6> LINE_PLANES{{
    {OUT_RIGHT, -1.0f, 0.0f, 0.0f, 1.0f},
    {OUT_LEFT, 1.0f, 0.0f, 0.0f, 1.0f},
    {OUT_TOP, 0.0f, -1.0f, 0.0f, 1.0f},
    {OUT_BOTTOM, 0.0f, 1.0f, 0.0f, 1.0f},
    {OUT_NEAR, 0.0f, 0.0f, -1.0f, 1.0f},
    {OUT_FAR, 0.0f, 0.0f, 1.0f, 1.0f},
}};

// Texture coordinate offsets selected by the line/point offset fields of BP LPSIZE.
constexpr std::array<float, 8> TEXCOORD_OFFSETS{0.0f, 0.0625f, 0.125f, 0.25f,
                                                0.5f, 1.0f,    1.0f,   1.0f};

// Storage for vertices made by clipping. Six planes add at most two vertices each to a convex
// polygon; the sizes leave room for rounding making it slightly non-convex.
constexpr int SCRATCH_SIZE = 64;
std::array<Vertex, SCRATCH_SIZE> s_scratch;
int s_scratch_used = 0;

Vertex* NewScratchVertex()
{
  // Only a degenerate polygon could run out; it then reuses the last slot.
  if (s_scratch_used == SCRATCH_SIZE)
    return &s_scratch.back();
  return &s_scratch[static_cast<size_t>(s_scratch_used++)];
}

float PlaneDistance(const Vertex& v, const ClipPlane& plane)
{
  const Common::Vec4& p = v.projectedPosition;
  return p.x * plane.cx + p.y * plane.cy + p.z * plane.cz + p.w * plane.cw;
}

u32 OutCode(const Vertex& v)
{
  const Common::Vec4& p = v.projectedPosition;
  u32 code = 0;
  if (p.w - p.x < 0)
    code |= OUT_RIGHT;
  if (p.x + p.w < 0)
    code |= OUT_LEFT;
  if (p.w - p.y < 0)
    code |= OUT_TOP;
  if (p.y + p.w < 0)
    code |= OUT_BOTTOM;
  if (p.w * p.z > 0)
    code |= OUT_NEAR;
  if (p.z + p.w < 0)
    code |= OUT_FAR;
  return code;
}

// Fills every attribute the later stages read with the point at t on the way from a to b.
// The window position is left alone; the perspective divide computes it.
void Interpolate(Vertex* out, float t, const Vertex& a, const Vertex& b)
{
  const auto mix = [t](float from, float to) { return from + (to - from) * t; };
  const auto mix3 = [&mix](Common::Vec3& dst, const Common::Vec3& from, const Common::Vec3& to) {
    dst.x = mix(from.x, to.x);
    dst.y = mix(from.y, to.y);
    dst.z = mix(from.z, to.z);
  };

  mix3(out->mvPosition, a.mvPosition, b.mvPosition);

  out->projectedPosition.x = mix(a.projectedPosition.x, b.projectedPosition.x);
  out->projectedPosition.y = mix(a.projectedPosition.y, b.projectedPosition.y);
  out->projectedPosition.z = mix(a.projectedPosition.z, b.projectedPosition.z);
  out->projectedPosition.w = mix(a.projectedPosition.w, b.projectedPosition.w);

  for (size_t n = 0; n < out->normal.size(); ++n)
    mix3(out->normal[n], a.normal[n], b.normal[n]);

  // Colours blend in 8-bit fixed point.
  const int weight = static_cast<u16>(t * 256.0f);
  for (size_t chan = 0; chan < out->color.size(); ++chan)
  {
    for (size_t comp = 0; comp < 4; ++comp)
    {
      const int from = a.color[chan][comp];
      const int to = b.color[chan][comp];
      out->color[chan][comp] = static_cast<u8>(from + (((to - from) * weight) >> 8));
    }
  }

  for (size_t n = 0; n < out->texCoords.size(); ++n)
    mix3(out->texCoords[n], a.texCoords[n], b.texCoords[n]);
}

// A triangle clipped by six planes has at most nine vertices.
constexpr size_t MAX_POLYGON = 32;

struct Polygon
{
  std::array<Vertex*, MAX_POLYGON> v{};
  size_t count = 0;
};

// Sutherland-Hodgman step against one plane. The walk starts with the edge from the first vertex
// to the second, which decides the order of the output and with it the fan triangulation.
void ClipAgainstPlane(const ClipPlane& plane, const Polygon& in, Polygon& out)
{
  out.count = 0;
  Vertex* start = in.v[0];
  float start_dist = PlaneDistance(*start, plane);

  for (size_t i = 1; i <= in.count && out.count + 2 <= MAX_POLYGON; ++i)
  {
    Vertex* end = in.v[i == in.count ? 0 : i];
    const float end_dist = PlaneDistance(*end, plane);

    if (start_dist >= 0)
      out.v[out.count++] = start;

    // The edge crosses the plane when one end is above zero and the other is not.
    const bool crosses = (end_dist <= 0 && start_dist > 0) || (end_dist > 0 && start_dist <= 0);
    if (crosses)
    {
      Vertex* cut = NewScratchVertex();
      if (end_dist < 0)
        Interpolate(cut, end_dist / (end_dist - start_dist), *end, *start);
      else
        Interpolate(cut, start_dist / (start_dist - end_dist), *start, *end);
      out.v[out.count++] = cut;
    }

    start = end;
    start_dist = end_dist;
  }
}

void DivideAndDraw(Vertex* a, Vertex* b, Vertex* c)
{
  PerspectiveDivide(a);
  PerspectiveDivide(b);
  PerspectiveDivide(c);
  Rasterizer::DrawTriangleFrontFace(a, b, c);
}

// Clips a front-facing triangle and draws what is left as a fan around its first vertex.
void ClipAndDraw(Vertex* a, Vertex* b, Vertex* c)
{
  const u32 outside = OutCode(*a) | OutCode(*b) | OutCode(*c);
  if (outside == 0)
  {
    DivideAndDraw(a, b, c);
    return;
  }

  s_scratch_used = 0;
  std::array<Polygon, 2> buffers;
  buffers[0].v[0] = a;
  buffers[0].v[1] = b;
  buffers[0].v[2] = c;
  buffers[0].count = 3;
  size_t current = 0;

  for (const ClipPlane& plane : TRIANGLE_PLANES)
  {
    if ((outside & plane.outcode) == 0)
      continue;
    Polygon& next = buffers[current ^ 1];
    ClipAgainstPlane(plane, buffers[current], next);
    if (next.count < 3)
      return;  // nothing visible remains
    current ^= 1;
  }

  INCSTAT(g_stats.this_frame.num_triangles_clipped);

  const Polygon& poly = buffers[current];
  for (size_t i = 2; i < poly.count; ++i)
    DivideAndDraw(poly.v[0], poly.v[i - 1], poly.v[i]);
}

// Copies a vertex for the triangles that make up a wide line or a point, moving its window
// position by (sign_x, sign_y) times half_size.
void CopyOffset(Vertex* dst, const Vertex& src, float sign_x, float sign_y, float half_size)
{
  dst->projectedPosition = src.projectedPosition;
  dst->screenPosition.x = src.screenPosition.x + sign_x * half_size;
  dst->screenPosition.y = src.screenPosition.y + sign_y * half_size;
  dst->screenPosition.z = src.screenPosition.z;
  dst->normal = src.normal;
  dst->color = src.color;
  dst->texCoords = src.texCoords;
}

float TexScaleS(u32 coord)
{
  return static_cast<float>(bpmem.texcoords[coord].s.scale_minus_1 + 1);
}

float TexScaleT(u32 coord)
{
  return static_cast<float>(bpmem.texcoords[coord].t.scale_minus_1 + 1);
}

void MakeLineVertex(Vertex* dst, const Vertex& src, int px, int py, bool far_side)
{
  CopyOffset(dst, src, static_cast<float>(px), static_cast<float>(py),
             static_cast<float>(bpmem.lineptwidth.linesize) / 12.0f);

  const float offset = TEXCOORD_OFFSETS[bpmem.lineptwidth.lineoff];
  if (!far_side || offset == 0)
    return;
  for (u32 coord = 0; coord < xfmem.numTexGen.numTexGens; ++coord)
  {
    if (bpmem.texcoords[coord].s.line_offset)
      dst->texCoords[coord].x += TexScaleS(coord) * offset;
  }
}

void MakePointVertex(Vertex* dst, const Vertex& src, bool right, bool up)
{
  CopyOffset(dst, src, right ? 1.0f : -1.0f, up ? 1.0f : -1.0f,
             static_cast<float>(bpmem.lineptwidth.pointsize) / 12.0f);

  const float offset = TEXCOORD_OFFSETS[bpmem.lineptwidth.pointoff];
  if (offset == 0)
    return;
  for (u32 coord = 0; coord < xfmem.numTexGen.numTexGens; ++coord)
  {
    if (!bpmem.texcoords[coord].s.point_offset)
      continue;
    if (right)
      dst->texCoords[coord].x += TexScaleS(coord) * offset;
    if (up)
      dst->texCoords[coord].y += TexScaleT(coord) * offset;
  }
}
}  // namespace

void Init()
{
  s_scratch_used = 0;
}

void ProcessTriangle(OutputVertexData* v0, OutputVertexData* v1, OutputVertexData* v2)
{
  INCSTAT(g_stats.this_frame.num_triangles_in);

  if (IsTriviallyRejected(v0, v1, v2))
  {
    // A rejected triangle does not touch the depth slope that z-freeze uses.
    INCSTAT(g_stats.this_frame.num_triangles_rejected);
    return;
  }

  const bool back = IsBackface(v0, v1, v2);
  // Back faces are drawn with their winding reversed so that the rasterizer only sees front faces.
  Vertex* const first = v0;
  Vertex* const second = back ? v2 : v1;
  Vertex* const third = back ? v1 : v2;

  const CullMode cull = bpmem.genMode.cull_mode;
  const bool culled =
      cull == CullMode::All || (back ? cull == CullMode::Front : cull == CullMode::Back);
  if (culled)
  {
    // Culled triangles still set the depth slope for z-freeze.
    PerspectiveDivide(first);
    PerspectiveDivide(second);
    PerspectiveDivide(third);
    Rasterizer::UpdateZSlope(first, second, third, static_cast<s32>(bpmem.scissorOffset.x * 2),
                             static_cast<s32>(bpmem.scissorOffset.y * 2));
    INCSTAT(g_stats.this_frame.num_triangles_culled);
    return;
  }

  // With clipping detection disabled the triangle is drawn unclipped, unless a vertex is behind
  // the eye (negative w); the divide would mirror it, so that case is still clipped.
  // Hardware has some guard band limit even then, which is not modelled.
  if (xfmem.clipDisable.disable_clipping_detection && first->projectedPosition.w >= 0 &&
      second->projectedPosition.w >= 0 && third->projectedPosition.w >= 0)
  {
    DivideAndDraw(first, second, third);
    return;
  }

  ClipAndDraw(first, second, third);
}

void ProcessLine(OutputVertexData* v0, OutputVertexData* v1)
{
  Vertex* start = v0;
  Vertex* end = v1;

  const u32 code0 = OutCode(*v0);
  const u32 code1 = OutCode(*v1);
  if ((code0 | code1) != 0)
  {
    // Parametric clipping: t_start and t_end are how far each end moves towards the other.
    float t_start = 0;
    float t_end = 0;
    for (const ClipPlane& plane : LINE_PLANES)
    {
      if (((code0 | code1) & plane.outcode) == 0)
        continue;
      const float d0 = PlaneDistance(*v0, plane);
      const float d1 = PlaneDistance(*v1, plane);
      const bool out0 = d0 < 0;
      const bool out1 = d1 < 0;
      if (out0 && out1)
        return;
      if (out1)
      {
        const float t = d1 / (d1 - d0);
        if (t > t_end)
          t_end = t;
      }
      else if (out0)
      {
        const float t = d0 / (d0 - d1);
        if (t > t_start)
          t_start = t;
      }
    }

    s_scratch_used = 0;
    if (code0 != 0)
    {
      start = NewScratchVertex();
      Interpolate(start, t_start, *v0, *v1);
    }
    if (code1 != 0)
    {
      end = NewScratchVertex();
      Interpolate(end, t_end, *v1, *v0);
    }
  }

  PerspectiveDivide(start);
  PerspectiveDivide(end);

  // Lines are drawn as quads whose short sides are vertical or horizontal, depending on which
  // way the line is steeper, rather than perpendicular to the line. The signs keep the two
  // triangles counter-clockwise.
  const float dx = end->screenPosition.x - start->screenPosition.x;
  const float dy = end->screenPosition.y - start->screenPosition.y;
  int px = 0;
  int py = 0;
  if (std::fabs(dx) > std::fabs(dy))
    py = dx > 0 ? -1 : 1;
  else
    px = dy > 0 ? 1 : -1;

  std::array<Vertex, 3> quad;
  MakeLineVertex(&quad[0], *start, px, py, false);
  MakeLineVertex(&quad[1], *end, px, py, false);
  MakeLineVertex(&quad[2], *end, -px, -py, true);
  Rasterizer::DrawTriangleFrontFace(&quad[2], &quad[1], &quad[0]);

  MakeLineVertex(&quad[1], *start, -px, -py, true);
  Rasterizer::DrawTriangleFrontFace(&quad[0], &quad[1], &quad[2]);
}

void ProcessPoint(OutputVertexData* center)
{
  // Points are not clipped.
  PerspectiveDivide(center);

  Vertex lower_left, lower_right, upper_left, upper_right;
  MakePointVertex(&lower_left, *center, false, false);
  MakePointVertex(&lower_right, *center, true, false);
  MakePointVertex(&upper_right, *center, true, true);
  MakePointVertex(&upper_left, *center, false, true);

  Rasterizer::DrawTriangleFrontFace(&lower_left, &upper_left, &lower_right);
  Rasterizer::DrawTriangleFrontFace(&upper_right, &lower_right, &upper_left);
}

bool IsTriviallyRejected(const OutputVertexData* v0, const OutputVertexData* v1,
                         const OutputVertexData* v2)
{
  return (OutCode(*v0) & OutCode(*v1) & OutCode(*v2)) != 0;
}

bool IsBackface(const OutputVertexData* v0, const OutputVertexData* v1, const OutputVertexData* v2)
{
  // Sign of the triangle's orientation in homogeneous coordinates (the determinant of the x, y, w
  // columns), expanded along the second vertex.
  const Common::Vec4& a = v0->projectedPosition;
  const Common::Vec4& b = v1->projectedPosition;
  const Common::Vec4& c = v2->projectedPosition;
  const float minor_y = a.x * c.w - c.x * a.w;
  const float minor_w = c.x * a.y - a.x * c.y;
  const float minor_x = c.y * a.w - a.y * c.w;
  const float orientation = minor_y * b.y + minor_w * b.w + minor_x * b.x;

  // Most games use a negative viewport height, which mirrors y. A positive one (for example
  // Jimmie Johnson's Anything with an Engine) flips the meaning of the sign.
  const bool facing_away = orientation <= 0.0f;
  return xfmem.viewport.ht > 0 ? !facing_away : facing_away;
}

void PerspectiveDivide(OutputVertexData* vertex)
{
  const Common::Vec4& clip = vertex->projectedPosition;
  const float inv_w = 1.0f / clip.w;
  vertex->screenPosition.x = clip.x * inv_w * xfmem.viewport.wd + xfmem.viewport.xOrig;
  vertex->screenPosition.y = clip.y * inv_w * xfmem.viewport.ht + xfmem.viewport.yOrig;
  vertex->screenPosition.z = clip.z * inv_w * xfmem.viewport.zRange + xfmem.viewport.farZ;
}
}  // namespace Clipper
