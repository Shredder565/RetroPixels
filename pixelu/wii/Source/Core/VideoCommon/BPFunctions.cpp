// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/BPFunctions.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/SmallVector.h"
#include "Core/System.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/EFBInterface.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace BPFunctions
{
void FlushPipeline()
{
  g_vertex_manager->Flush();
}

void SetGenerationMode()
{
  g_vertex_manager->SetRasterizationStateChanged();
}

void SetDepthMode()
{
  g_vertex_manager->SetDepthStateChanged();
}

void SetBlendMode()
{
  g_vertex_manager->SetBlendingStateChanged();
}

// ---------------------------------------------------------------------------------------------
// Scissor

int ScissorRect::GetArea() const
{
  return rect.GetWidth() * rect.GetHeight();
}

int ScissorResult::GetViewportArea(const ScissorRect& candidate) const
{
  // The candidate in scissor coordinates, clipped to the viewport (whose bounds are truncated).
  const auto clip = [](int value, float low, float high) {
    return std::clamp<int>(value, static_cast<int>(low), static_cast<int>(high));
  };
  const MathUtil::Rectangle<int>& r = candidate.rect;
  const int width = clip(r.right + candidate.x_off, viewport_left, viewport_right) -
                    clip(r.left + candidate.x_off, viewport_left, viewport_right);
  const int height = clip(r.bottom + candidate.y_off, viewport_top, viewport_bottom) -
                     clip(r.top + candidate.y_off, viewport_top, viewport_bottom);
  return width * height;
}

// Ordering for the candidate list: more of the viewport covered is better, then a larger total
// area. Sorting by this puts the preferred rectangle last, so drawing the list in order paints
// the preferred one on top. (Which order the hardware really uses has not been tested; it only
// matters for geometry outside the viewport.)
bool ScissorResult::IsWorse(const ScissorRect& lhs, const ScissorRect& rhs) const
{
  const int lhs_visible = GetViewportArea(lhs);
  const int rhs_visible = GetViewportArea(rhs);
  if (lhs_visible == rhs_visible)
    return lhs.GetArea() < rhs.GetArea();
  return lhs_visible < rhs_visible;
}

namespace
{
// Scissor coordinates wrap every 1024 units; nine wraps cover every possible overlap.
constexpr int WRAP = 1024;
constexpr int MAX_WRAPS = 4;

using RangeList = Common::SmallVector<ScissorRange, 2 * MAX_WRAPS + 1>;

// All non-empty EFB ranges hit by the closed scissor interval [first, last] under each wrap of
// `offset`.
RangeList ScissorAxis(int first, int last, int offset, int efb_size)
{
  RangeList hits;
  for (int wrap = -MAX_WRAPS; wrap <= MAX_WRAPS; ++wrap)
  {
    const int shift = offset + wrap * WRAP;
    const int begin = std::clamp(first - shift, 0, efb_size);
    const int end = std::clamp(last + 1 - shift, 0, efb_size);
    if (begin < end)
      hits.emplace_back(shift, begin, end);
  }
  return hits;
}

// The [min, max] pair of two values, ordered the way std::minmax orders them.
std::pair<float, float> OrderedPair(float a, float b)
{
  if (b < a)
    return {b, a};
  return {a, b};
}
}  // namespace

ScissorResult::ScissorResult(ScissorPos scissor_top_left, ScissorPos scissor_bottom_right,
                             ScissorOffset scissor_offset, const Viewport& viewport)
    : ScissorResult(scissor_top_left, scissor_bottom_right, scissor_offset,
                    OrderedPair(viewport.xOrig - viewport.wd, viewport.xOrig + viewport.wd),
                    OrderedPair(viewport.yOrig - viewport.ht, viewport.yOrig + viewport.ht))
{
}

ScissorResult::ScissorResult(ScissorPos scissor_top_left, ScissorPos scissor_bottom_right,
                             ScissorOffset scissor_offset, std::pair<float, float> viewport_x,
                             std::pair<float, float> viewport_y)
    : scissor_tl{.hex = scissor_top_left.hex}, scissor_br{.hex = scissor_bottom_right.hex},
      scissor_off{.hex = scissor_offset.hex}, viewport_left(viewport_x.first),
      viewport_right(viewport_x.second), viewport_top(viewport_y.first),
      viewport_bottom(viewport_y.second)
{
  // Inclusive scissor bounds. An inverted rectangle draws nothing, whatever the offset.
  const int x_first = static_cast<int>(scissor_tl.x.Value());
  const int x_last = static_cast<int>(scissor_br.x.Value());
  const int y_first = static_cast<int>(scissor_tl.y.Value());
  const int y_last = static_cast<int>(scissor_br.y.Value());
  if (x_first > x_last || y_first > y_last)
    return;

  // The SDK biases both coordinates and offsets by 342 (offsets before halving); the biases
  // cancel when the offset is subtracted, and must stay in for the inversion test above.
  const RangeList columns =
      ScissorAxis(x_first, x_last, static_cast<int>(scissor_off.x << 1), EFB_WIDTH);
  const RangeList rows =
      ScissorAxis(y_first, y_last, static_cast<int>(scissor_off.y << 1), EFB_HEIGHT);

  // Every column range combined with every row range; each result is half-open.
  rectangles.reserve(columns.size() * rows.size());
  for (const ScissorRange& column : columns)
  {
    DEBUG_ASSERT(column.start < column.end);
    DEBUG_ASSERT(static_cast<u32>(column.end) <= EFB_WIDTH);
    for (const ScissorRange& row : rows)
    {
      DEBUG_ASSERT(row.start < row.end);
      DEBUG_ASSERT(static_cast<u32>(row.end) <= EFB_HEIGHT);
      rectangles.emplace_back(column, row);
    }
  }

  std::ranges::sort(rectangles, [this](const ScissorRect& lhs, const ScissorRect& rhs) {
    return IsWorse(lhs, rhs);
  });
}

ScissorRect ScissorResult::Best() const
{
  if (rectangles.empty())
  {
    // Nothing is visible: hand back a rectangle entirely outside the EFB.
    constexpr ScissorRange off_screen{0, 1000, 1001};
    return ScissorRect(off_screen, off_screen);
  }
  return rectangles.back();
}

ScissorResult ComputeScissorRects(ScissorPos scissor_top_left, ScissorPos scissor_bottom_right,
                                  ScissorOffset scissor_offset, const Viewport& viewport)
{
  return ScissorResult(scissor_top_left, scissor_bottom_right, scissor_offset, viewport);
}

// ---------------------------------------------------------------------------------------------
// Viewport

void SetScissorAndViewport(FramebufferManager* frame_buffer_manager, ScissorPos scissor_top_left,
                           ScissorPos scissor_bottom_right, ScissorOffset scissor_offset,
                           Viewport viewport)
{
  const ScissorRect chosen =
      ComputeScissorRects(scissor_top_left, scissor_bottom_right, scissor_offset, viewport).Best();

  const auto target_rect = frame_buffer_manager->ConvertEFBRectangle(chosen.rect);
  const auto scissor_rect =
      g_gfx->ConvertFramebufferRectangle(target_rect, g_gfx->GetCurrentFramebuffer());
  g_gfx->SetScissorRect(scissor_rect);

  // XF stores the viewport as centre and half extents (height negated); the scissor offset that
  // selected the rectangle moves the origin too.
  std::array<float, 4> efb_viewport = {
      (viewport.xOrig - static_cast<float>(chosen.x_off)) - viewport.wd,
      (viewport.yOrig - static_cast<float>(chosen.y_off)) + viewport.ht,
      2.0f * viewport.wd,
      -2.0f * viewport.ht,
  };
  // With vertex rounding the viewport is snapped to whole native pixels as well (this removes a
  // seam in at least one game at high internal resolutions).
  if (g_ActiveConfig.UseVertexRounding())
  {
    for (float& v : efb_viewport)
      v = std::round(v);
  }

  float x = frame_buffer_manager->EFBToScaledXf(efb_viewport[0]);
  float y = frame_buffer_manager->EFBToScaledYf(efb_viewport[1]);
  float width = frame_buffer_manager->EFBToScaledXf(efb_viewport[2]);
  float height = frame_buffer_manager->EFBToScaledYf(efb_viewport[3]);
  // Make the extents positive by moving the origin to the other edge.
  if (width < 0.f)
  {
    x += width;
    width = -width;
  }
  if (height < 0.f)
  {
    y += height;
    height = -height;
  }

  // XF depth is in 24-bit units.
  constexpr float DEPTH_UNITS = 16777216.0f;
  float min_depth = (viewport.farZ - viewport.zRange) / DEPTH_UNITS;
  float max_depth = viewport.farZ / DEPTH_UNITS;

  // Without depth clamping an oversized range cannot be represented; keep it within what the
  // console supports.
  if (!g_backend_info.bSupportsDepthClamp)
  {
    min_depth = std::clamp(min_depth, 0.0f, MAX_EFB_DEPTH);
    max_depth = std::clamp(max_depth, 0.0f, MAX_EFB_DEPTH);
  }

  // When the vertex shader applies the depth range itself, the viewport only has to cover the
  // console's full range (reversed when the game uses a negative range and the API allows it).
  if (VertexShaderManager::UseVertexDepthRange())
  {
    const bool reversed = viewport.zRange < 0.0f && g_backend_info.bSupportsReversedDepthRange;
    min_depth = reversed ? MAX_EFB_DEPTH : 0.0f;
    max_depth = reversed ? 0.0f : MAX_EFB_DEPTH;
  }

  // Depth is always reversed on the host (1 near, 0 far), which matches the precision of the
  // console's clip range: directly when the API supports a reversed range, otherwise by
  // flipping the values.
  const bool api_reverses = g_backend_info.bSupportsReversedDepthRange;
  const float near_depth = api_reverses ? max_depth : 1.0f - max_depth;
  const float far_depth = api_reverses ? min_depth : 1.0f - min_depth;

  if (g_backend_info.bUsesLowerLeftOrigin)
    y = static_cast<float>(g_gfx->GetCurrentFramebuffer()->GetHeight()) - y - height;

  g_gfx->SetViewport(x, y, width, height, near_depth, far_depth);
  g_gfx->StoreViewportAndScissor(AbstractGfx::ViewportAndScissor{
      .scissor_rect = scissor_rect,
      .viewport_x = x,
      .viewport_y = y,
      .viewport_width = width,
      .viewport_height = height,
      .viewport_near_depth = near_depth,
      .viewport_far_depth = far_depth,
  });
}

// ---------------------------------------------------------------------------------------------
// EFB clears and format changes

// The host EFB is always RGBA8 with 24-bit depth, so clears are adjusted to what the emulated
// format can hold: formats without alpha never get their alpha cleared (it must stay at its
// initial value), and colour/depth are reduced to the format's precision before being written.
bool ClearScreen(FramebufferManager* frame_buffer_manager, const MathUtil::Rectangle<int>& rc,
                 bool color_enable, bool alpha_enable, bool z_enable, PixelFormat pixel_format,
                 u32 clear_color_ar, u32 clear_color_gb, u32 clear_z_value)
{
  const bool format_has_alpha = pixel_format != PixelFormat::RGB8_Z24 &&
                                pixel_format != PixelFormat::RGB565_Z16 &&
                                pixel_format != PixelFormat::Z24;
  alpha_enable = alpha_enable && format_has_alpha;

  if (!color_enable && !alpha_enable && !z_enable)
    return false;

  u32 color = (clear_color_ar << 16) | clear_color_gb;
  u32 depth = clear_z_value;
  switch (pixel_format)
  {
  case PixelFormat::RGBA6_Z24:
    color = RGBA8ToRGBA6ToRGBA8(color);
    break;
  case PixelFormat::RGB565_Z16:
    color = RGBA8ToRGB565ToRGBA8(color);
    depth = Z24ToZ16ToZ24(depth);
    break;
  default:
    break;
  }

  frame_buffer_manager->ClearEFB(rc, color_enable, alpha_enable, z_enable, color, depth,
                                 pixel_format);
  return true;
}

namespace
{
// Colour layouts that need converting between; Z24 stores colour like RGB8_Z24.
enum class ColorLayout
{
  RGB8,
  RGBA6,
  RGB565,
};

std::optional<ColorLayout> LayoutOf(PixelFormat format)
{
  switch (format)
  {
  case PixelFormat::RGB8_Z24:
  case PixelFormat::Z24:
    return ColorLayout::RGB8;
  case PixelFormat::RGBA6_Z24:
    return ColorLayout::RGBA6;
  case PixelFormat::RGB565_Z16:
    return ColorLayout::RGB565;
  default:
    return std::nullopt;
  }
}

EFBReinterpretType ConversionBetween(ColorLayout from, ColorLayout to)
{
  switch (from)
  {
  case ColorLayout::RGB8:
    return to == ColorLayout::RGBA6 ? EFBReinterpretType::RGB8ToRGBA6 :
                                      EFBReinterpretType::RGB8ToRGB565;
  case ColorLayout::RGBA6:
    return to == ColorLayout::RGB8 ? EFBReinterpretType::RGBA6ToRGB8 :
                                     EFBReinterpretType::RGBA6ToRGB565;
  case ColorLayout::RGB565:
  default:
    return to == ColorLayout::RGB8 ? EFBReinterpretType::RGB565ToRGB8 :
                                     EFBReinterpretType::RGB565ToRGBA6;
  }
}
}  // namespace

// Changing the EFB format on the console does not touch the stored pixels; they are simply read
// in the new layout. Since the host buffer is RGBA8, the existing contents are reinterpreted to
// get the same effect. (Depth is always 24-bit here, and Z compression formats are ignored.)
void OnPixelFormatChange(FramebufferManager* frame_buffer_manager, PixelFormat pixel_format,
                         DepthFormat z_format)
{
  if (!g_ActiveConfig.bEFBEmulateFormatChanges)
    return;

  const PixelFormat previous = frame_buffer_manager->GetPrevPixelFormat();
  frame_buffer_manager->StorePixelFormat(pixel_format);

  DEBUG_LOG_FMT(VIDEO, "pixelfmt: pixel={}, zc={}", pixel_format, z_format);

  if (pixel_format == previous || previous == PixelFormat::INVALID_FMT)
    return;

  const std::optional<ColorLayout> from = LayoutOf(previous);
  const std::optional<ColorLayout> to = LayoutOf(pixel_format);
  if (!from || !to)
  {
    ERROR_LOG_FMT(VIDEO, "Unhandled EFB format change: {} to {}", previous, pixel_format);
    return;
  }
  if (*from != *to)
    g_efb_interface->ReinterpretPixelData(ConversionBetween(*from, *to));
}

void SetInterlacingMode(const BPCmd& bp)
{
  // Field rendering is not emulated; just log what the game asks for.
  if (bp.address == BPMEM_FIELDMODE)
  {
    // The SDK sets lineptwidth.adjust_for_aspect_ratio right before this register.
    DEBUG_LOG_FMT(VIDEO, "BPMEM_FIELDMODE texLOD:{} lineaspect:{}", bpmem.fieldmode.texLOD,
                  bpmem.lineptwidth.adjust_for_aspect_ratio);
  }
  else if (bp.address == BPMEM_FIELDMASK)
  {
    DEBUG_LOG_FMT(VIDEO, "BPMEM_FIELDMASK even:{} odd:{}", bpmem.fieldmask.even,
                  bpmem.fieldmask.odd);
  }
  else
  {
    ERROR_LOG_FMT(VIDEO, "SetInterlacingMode default");
  }
}
}  // namespace BPFunctions
