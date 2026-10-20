// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Software/Rasterizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"

#include "VideoBackends/Software/NativeVertexFormat.h"
#include "VideoBackends/Software/SWEfbInterface.h"
#include "VideoBackends/Software/Tev.h"

#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/PerfQueryBase.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/XFMemory.h"

namespace Rasterizer
{
namespace
{
// Pixels are shaded in square blocks of this many pixels per side, so that texture LODs can be
// taken from screen-space differences inside the block.
constexpr s32 QUAD = 2;

// Rounds to the nearest integer, halves away from the truncation.
s32 RoundNearest(float value)
{
  const s32 truncated = static_cast<s32>(value);
  return value - static_cast<float>(truncated) >= 0.5 ? truncated + 1 : truncated;
}

// Converts a screen coordinate to the 28.4 fixed point the edge functions use, including the
// hardware's sub-pixel bias.
s32 ToFixed(float coordinate, s32 offset)
{
  return RoundNearest(16.0f * (coordinate - static_cast<float>(offset))) - 9;
}

// Turns a 28.4 fixed point coordinate into the first pixel at or after it.
s32 PixelCeil(s32 fixed)
{
  return (fixed + 0xF) >> 4;
}

// Where interpolation starts for a triangle: its first vertex, snapped to a pixel, plus the
// sub-pixel distance from that pixel's sample point to the real vertex.
struct Origin
{
  Origin(const OutputVertexData& v0, const OutputVertexData& v1, const OutputVertexData& v2,
         s32 pixel_x, s32 pixel_y, s32 x_off, s32 y_off)
      : px(pixel_x), py(pixel_y)
  {
    // Sample a little short of the pixel centre.
    constexpr float SAMPLE_BIAS = 0.495f;
    const float vertex_x = v0.screenPosition.x - static_cast<float>(x_off);
    fx = (static_cast<float>(pixel_x) - vertex_x) + SAMPLE_BIAS;
    const float vertex_y = v0.screenPosition.y - static_cast<float>(y_off);
    fy = (static_cast<float>(pixel_y) - vertex_y) + SAMPLE_BIAS;

    ex1 = v1.screenPosition.x - v0.screenPosition.x;
    ex2 = v2.screenPosition.x - v0.screenPosition.x;
    ey1 = v1.screenPosition.y - v0.screenPosition.y;
    ey2 = v2.screenPosition.y - v0.screenPosition.y;
  }

  s32 px, py;                // the starting pixel
  float fx, fy;              // fractional offset of the sample from vertex 0
  float ex1, ex2, ey1, ey2;  // edges v0->v1 and v0->v2
};

// A linear function of screen position, set up from its values at the triangle's vertices.
struct Plane
{
  Plane() = default;
  Plane(float at0, float at1, float at2, const Origin& origin)
      : base(at0), px(origin.px), py(origin.py), fx(origin.fx), fy(origin.fy)
  {
    const float d1 = at1 - at0;
    const float d2 = at2 - at0;
    const float num_x = d2 * origin.ey1 - d1 * origin.ey2;
    const float num_y = origin.ex2 * d1 - origin.ex1 * d2;
    const float area = origin.ex2 * origin.ey1 - origin.ex1 * origin.ey2;
    step_x = num_x / area;
    step_y = num_y / area;
  }

  float At(s32 x, s32 y) const
  {
    const float dx = fx + static_cast<float>(x - px);
    const float dy = fy + static_cast<float>(y - py);
    return base + (step_x * dx) + (step_y * dy);
  }

  // The defaults only matter when z-freeze is already on for the very first primitive; what the
  // hardware does there is unknown.
  float step_x = 0.0f;
  float step_y = 0.0f;
  float base = 1.0f;
  s32 px = 0;
  s32 py = 0;
  float fx = 0.0f;
  float fy = 0.0f;
};

// One half-plane of a triangle in 28.4 fixed point: Eval() > 0 means inside. Evaluated with
// wrapping arithmetic so that extreme coordinates cannot overflow.
struct HalfPlane
{
  HalfPlane(s32 from_x, s32 from_y, s32 to_x, s32 to_y)
      : dx(static_cast<u32>(from_x) - static_cast<u32>(to_x)),
        dy(static_cast<u32>(from_y) - static_cast<u32>(to_y))
  {
    bias = dy * static_cast<u32>(from_x) - dx * static_cast<u32>(from_y);
    // Top-left fill rule: pixels exactly on a top or left edge belong to the triangle.
    const s32 sdx = static_cast<s32>(dx);
    const s32 sdy = static_cast<s32>(dy);
    if (sdy < 0 || (sdy == 0 && sdx > 0))
      ++bias;
  }

  bool Inside(s32 pixel_x, s32 pixel_y) const
  {
    const u32 fixed_x = static_cast<u32>(pixel_x) << 4;
    const u32 fixed_y = static_cast<u32>(pixel_y) << 4;
    return static_cast<s32>(bias + dx * fixed_y - dy * fixed_x) > 0;
  }

  // Bit n set when corner n of the block at (x, y) is inside (0 top-left, 1 top-right,
  // 2 bottom-left, 3 bottom-right).
  u32 BlockMask(s32 x, s32 y) const
  {
    const s32 x_last = x + QUAD - 1;
    const s32 y_last = y + QUAD - 1;
    return (Inside(x, y) ? 1u : 0u) | (Inside(x_last, y) ? 2u : 0u) |
           (Inside(x, y_last) ? 4u : 0u) | (Inside(x_last, y_last) ? 8u : 0u);
  }

  u32 dx;
  u32 dy;
  u32 bias = 0;
};

// Per-pixel texture coordinates of the block being shaded, plus the per-block texture LODs.
// Entries past the active counts keep whatever an earlier block left in them.
struct Block
{
  std::array<std::array<std::array<std::array<float, 2>, 8>, QUAD>, QUAD> uv{};  // [x][y][coord]
  std::array<s32, 4> indirect_lod{};
  std::array<bool, 4> indirect_linear{};
  std::array<s32, 16> lod{};
  std::array<bool, 16> linear{};
};

Plane s_depth;
Plane s_inv_w;
std::array<std::array<Plane, 4>, 2> s_color;
std::array<std::array<Plane, 3>, 8> s_texcoord;

Tev s_tev;
Block s_block;

std::vector<BPFunctions::ScissorRect> s_scissors;

// log2(value) in s28.4, from the float's exponent and the top bits of its mantissa. Accurate
// enough for picking mip levels.
s32 ApproxLog2(float value)
{
  u32 bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const s32 whole = static_cast<s32>(((bits & 0x7F800000) >> 19) - 2032);
  const s32 fraction = static_cast<s32>((bits & 0x007fffff) >> 19);
  return whole + fraction;
}

// The LOD and filter for texture map `texmap` sampled with coordinate `texcoord`, from the
// coordinate's change across the current block. Only the mode registers matter, not the size
// of the texture.
void ComputeLod(s32& lod_out, bool& linear_out, u32 texmap, u32 texcoord)
{
  const auto unit = bpmem.tex.GetUnit(texmap);
  const TexMode0& mode = unit.texMode0;
  const TexMode1& limits = unit.texMode1;

  const auto& here = s_block.uv[0][0][texcoord];
  const auto& right = s_block.uv[1][0][texcoord];
  const auto& below = s_block.uv[0][1][texcoord];
  const float du_x = std::fabs(here[0] - right[0]);
  const float dv_x = std::fabs(here[1] - right[1]);
  const float du_y = std::fabs(here[0] - below[0]);
  const float dv_y = std::fabs(here[1] - below[1]);

  float s_change;
  float t_change;
  if (mode.diag_lod == LODType::Diagonal)
  {
    s_change = du_x + du_y;
    t_change = dv_x + dv_y;
  }
  else
  {
    s_change = std::max(du_x, du_y);
    t_change = std::max(dv_x, dv_y);
  }

  // The bias register is s2.5; the LOD is s28.4.
  const s32 bias = mode.lod_bias;
  s32 lod = ApproxLog2(std::max(s_change, t_change)) + (bias >> 1);

  linear_out =
      lod > 0 ? mode.min_filter == FilterMode::Linear : mode.mag_filter == FilterMode::Linear;

  // The maximum wins when min > max, so it has to be checked first.
  const s32 max_lod = static_cast<s32>(limits.max_lod);
  const s32 min_lod = static_cast<s32>(limits.min_lod);
  if (lod > max_lod)
    lod = max_lod;
  else if (lod < min_lod)
    lod = min_lod;
  lod_out = lod;
}

// Interpolates the perspective-correct texture coordinates of the block at (x, y), then the
// LODs of every indirect and TEV stage lookup.
void PrepareBlock(s32 x, s32 y)
{
  const u32 texgens = bpmem.genMode.numtexgens;
  for (s32 by = 0; by < QUAD; ++by)
  {
    for (s32 bx = 0; bx < QUAD; ++bx)
    {
      const s32 px = x + bx;
      const s32 py = y + by;
      const float inv_w = 1.0f / s_inv_w.At(px, py);
      auto& uvs = s_block.uv[static_cast<size_t>(bx)][static_cast<size_t>(by)];
      for (u32 i = 0; i < texgens; ++i)
      {
        const auto& planes = s_texcoord[i];
        const float q = planes[2].At(px, py) * inv_w;
        const float scale = q != 0.0f ? inv_w / q : inv_w;
        uvs[i][0] = planes[0].At(px, py) * scale;
        uvs[i][1] = planes[1].At(px, py) * scale;
      }
    }
  }

  for (u32 i = 0; i < bpmem.genMode.numindstages; ++i)
  {
    const int stage = static_cast<int>(i);
    ComputeLod(s_block.indirect_lod[i], s_block.indirect_linear[i],
               bpmem.tevindref.getTexMap(stage), bpmem.tevindref.getTexCoord(stage));
  }

  for (u32 stage = 0; stage <= bpmem.genMode.numtevstages; ++stage)
  {
    const TwoTevStageOrders& orders = bpmem.tevorders[stage / 2];
    const int odd = static_cast<int>(stage & 1);
    if (!orders.getEnable(odd))
      continue;
    ComputeLod(s_block.lod[stage], s_block.linear[stage], orders.getTexMap(odd),
               orders.getTexCoord(odd));
  }
}

// Shades pixel (x, y), which is pixel (bx, by) of the current block.
void ShadePixel(s32 x, s32 y, s32 bx, s32 by)
{
  INCSTAT(g_stats.this_frame.rasterized_pixels);

  const s32 depth = static_cast<s32>(std::clamp<float>(s_depth.At(x, y), 0.0f, 16777215.0f));

  if (bpmem.GetEmulatedZ() == EmulatedZ::Early)
  {
    // The counters run whether or not the test is enabled (unverified).
    EfbInterface::IncPerfCounterQuadCount(PQ_ZCOMP_INPUT_ZCOMPLOC);
    if (bpmem.zmode.test_enable &&
        !EfbInterface::ZCompare(static_cast<u16>(x), static_cast<u16>(y), static_cast<u32>(depth)))
    {
      return;
    }
    EfbInterface::IncPerfCounterQuadCount(PQ_ZCOMP_OUTPUT_ZCOMPLOC);
  }

  s_tev.Position = {x, y, depth};

  for (u32 chan = 0; chan < bpmem.genMode.numcolchans; ++chan)
  {
    for (size_t comp = 0; comp < 4; ++comp)
    {
      const float value = s_color[chan][comp].At(x, y);
      s_tev.Color[chan][comp] = static_cast<u8>(std::clamp<float>(value, 0.0f, 255.0f));
    }
  }

  // The TEV takes coordinates in s17.7, truncated to 24 bits.
  const auto& uvs = s_block.uv[static_cast<size_t>(bx)][static_cast<size_t>(by)];
  for (u32 i = 0; i < bpmem.genMode.numtexgens; ++i)
  {
    const s32 s = static_cast<s32>(uvs[i][0] * 128);
    const s32 t = static_cast<s32>(uvs[i][1] * 128);
    s_tev.Uv[i].s = static_cast<s32>(static_cast<u32>(s) << 8) >> 8;
    s_tev.Uv[i].t = static_cast<s32>(static_cast<u32>(t) << 8) >> 8;
  }

  for (u32 i = 0; i < bpmem.genMode.numindstages; ++i)
  {
    s_tev.IndirectLod[i] = s_block.indirect_lod[i];
    s_tev.IndirectLinear[i] = s_block.indirect_linear[i];
  }

  for (u32 stage = 0; stage <= bpmem.genMode.numtevstages; ++stage)
  {
    s_tev.TextureLod[stage] = s_block.lod[stage];
    s_tev.TextureLinear[stage] = s_block.linear[stage];
  }

  s_tev.Draw();
}

void Rasterize(const OutputVertexData& v0, const OutputVertexData& v1, const OutputVertexData& v2,
               const BPFunctions::ScissorRect& scissor)
{
  // z-freeze needs the slope of every triangle, including ones the scissor throws away.
  UpdateZSlope(&v0, &v1, &v2, scissor.x_off, scissor.y_off);

  const s32 x0 = ToFixed(v0.screenPosition.x, scissor.x_off);
  const s32 x1 = ToFixed(v1.screenPosition.x, scissor.x_off);
  const s32 x2 = ToFixed(v2.screenPosition.x, scissor.x_off);
  const s32 y0 = ToFixed(v0.screenPosition.y, scissor.y_off);
  const s32 y1 = ToFixed(v1.screenPosition.y, scissor.y_off);
  const s32 y2 = ToFixed(v2.screenPosition.y, scissor.y_off);

  ASSERT(scissor.rect.left >= 0);
  ASSERT(scissor.rect.right <= static_cast<int>(EFB_WIDTH));
  ASSERT(scissor.rect.top >= 0);
  ASSERT(scissor.rect.bottom <= static_cast<int>(EFB_HEIGHT));

  // Pixel bounds of the triangle, limited to the scissor rectangle (end exclusive).
  const s32 left = std::max(PixelCeil(std::min({x0, x1, x2})), scissor.rect.left);
  const s32 right = std::min(PixelCeil(std::max({x0, x1, x2})), scissor.rect.right);
  const s32 top = std::max(PixelCeil(std::min({y0, y1, y2})), scissor.rect.top);
  const s32 bottom = std::min(PixelCeil(std::max({y0, y1, y2})), scissor.rect.bottom);
  if (left >= right || top >= bottom)
    return;

  const Origin origin(v0, v1, v2, PixelCeil(x0), PixelCeil(y0), scissor.x_off, scissor.y_off);

  const std::array<float, 3> w = {1.0f / v0.projectedPosition.w, 1.0f / v1.projectedPosition.w,
                                  1.0f / v2.projectedPosition.w};
  s_inv_w = Plane(w[0], w[1], w[2], origin);

  for (u32 chan = 0; chan < bpmem.genMode.numcolchans; ++chan)
  {
    for (size_t comp = 0; comp < 4; ++comp)
    {
      s_color[chan][comp] =
          Plane(v0.color[chan][comp], v1.color[chan][comp], v2.color[chan][comp], origin);
    }
  }

  for (u32 i = 0; i < bpmem.genMode.numtexgens; ++i)
  {
    const auto& t0 = v0.texCoords[i];
    const auto& t1 = v1.texCoords[i];
    const auto& t2 = v2.texCoords[i];
    s_texcoord[i][0] = Plane(t0.x * w[0], t1.x * w[1], t2.x * w[2], origin);
    s_texcoord[i][1] = Plane(t0.y * w[0], t1.y * w[1], t2.y * w[2], origin);
    s_texcoord[i][2] = Plane(t0.z * w[0], t1.z * w[1], t2.z * w[2], origin);
  }

  const std::array<HalfPlane, 3> edges = {HalfPlane(x0, y0, x1, y1), HalfPlane(x1, y1, x2, y2),
                                          HalfPlane(x2, y2, x0, y0)};
  const auto covered = [&edges](s32 x, s32 y) {
    return edges[0].Inside(x, y) && edges[1].Inside(x, y) && edges[2].Inside(x, y);
  };

  for (s32 y = top & ~(QUAD - 1); y < bottom; y += QUAD)
  {
    for (s32 x = left & ~(QUAD - 1); x < right; x += QUAD)
    {
      const u32 m0 = edges[0].BlockMask(x, y);
      const u32 m1 = edges[1].BlockMask(x, y);
      const u32 m2 = edges[2].BlockMask(x, y);
      // Some edge has every corner of the block outside it.
      if (m0 == 0 || m1 == 0 || m2 == 0)
        continue;

      PrepareBlock(x, y);

      // A block with every corner inside every edge, and inside the scissor bounds, needs no
      // per-pixel tests.
      const bool whole = (m0 & m1 & m2) == 0xF && x >= left && x + QUAD - 1 < right && y >= top &&
                         y + QUAD - 1 < bottom;
      for (s32 by = 0; by < QUAD; ++by)
      {
        const s32 py = y + by;
        for (s32 bx = 0; bx < QUAD; ++bx)
        {
          const s32 px = x + bx;
          if (!whole && !(covered(px, py) && px >= left && px < right && py >= top && py < bottom))
            continue;
          ShadePixel(px, py, bx, by);
        }
      }
    }
  }
}
}  // namespace

void Init()
{
  // Every other plane is rebuilt for each triangle, but z-freeze can read the depth plane
  // before any triangle has set it.
  s_depth = Plane();
}

void ScissorChanged()
{
  s_scissors = BPFunctions::ComputeScissorRects(bpmem.scissorTL, bpmem.scissorBR,
                                                bpmem.scissorOffset, xfmem.viewport)
                   .rectangles;
}

void UpdateZSlope(const OutputVertexData* v0, const OutputVertexData* v1,
                  const OutputVertexData* v2, s32 x_off, s32 y_off)
{
  if (bpmem.genMode.zfreeze)
    return;
  const Origin origin(*v0, *v1, *v2, PixelCeil(ToFixed(v0->screenPosition.x, x_off)),
                      PixelCeil(ToFixed(v0->screenPosition.y, y_off)), x_off, y_off);
  s_depth = Plane(v0->screenPosition.z, v1->screenPosition.z, v2->screenPosition.z, origin);
}

void DrawTriangleFrontFace(const OutputVertexData* v0, const OutputVertexData* v1,
                           const OutputVertexData* v2)
{
  INCSTAT(g_stats.this_frame.num_triangles_drawn);
  for (const BPFunctions::ScissorRect& scissor : s_scissors)
    Rasterize(*v0, *v1, *v2, scissor);
}

void SetTevKonstColors()
{
  s_tev.SetKonstColors();
}
}  // namespace Rasterizer
