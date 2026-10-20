// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Translation of BP (pixel pipeline) register state into renderer calls, and the scissor maths
// shared with the statistics overlay.

#pragma once

#include <utility>
#include <vector>

#include "Common/MathUtil.h"
#include "VideoCommon/BPMemory.h"

class FramebufferManager;
struct Viewport;

namespace BPFunctions
{
// One axis of a scissor rectangle after applying a scissor offset: EFB coordinates
// [start, end) reached with the given total offset.
struct ScissorRange
{
  constexpr ScissorRange() = default;
  constexpr ScissorRange(int offset_, int start_, int end_)
      : offset(offset_), start(start_), end(end_)
  {
  }
  int offset = 0;
  int start = 0;
  int end = 0;
};

// A rectangle of the EFB the scissor lets through, plus the offset that maps it back to
// scissor coordinates.
struct ScissorRect
{
  constexpr ScissorRect(ScissorRange x_range, ScissorRange y_range)
      : rect(x_range.start, y_range.start, x_range.end, y_range.end), x_off(x_range.offset),
        y_off(y_range.offset)
  {
  }

  MathUtil::Rectangle<int> rect;
  int x_off;
  int y_off;

  int GetArea() const;
};

// The hardware has a single scissor rectangle, but its coordinates wrap: with a large enough
// offset (or a large enough rectangle) several separate parts of the EFB are drawn to. At least
// one commercial game relies on this during scrolling effects.
//
// Hardware backends can only take one rectangle, so all candidates are computed and sorted so
// that the best (the one covering most of the viewport) comes last; games are only known to
// intend a single visible rectangle.
//
// The scissor statistics overlay (OverlayScissorStats in GFX.ini) shows the inputs and the
// candidates.
struct ScissorResult
{
  ScissorResult(ScissorPos scissor_top_left, ScissorPos scissor_bottom_right,
                ScissorOffset scissor_offset, const Viewport& viewport);
  ~ScissorResult() = default;
  // The register unions cannot be assigned directly, hence the hand-written copies.
  ScissorResult(const ScissorResult& other) : rectangles{other.rectangles} { CopyInputs(other); }
  ScissorResult(ScissorResult&& other) : rectangles{std::move(other.rectangles)}
  {
    CopyInputs(other);
  }
  ScissorResult& operator=(const ScissorResult& other)
  {
    if (this != &other)
    {
      CopyInputs(other);
      rectangles = other.rectangles;
    }
    return *this;
  }
  ScissorResult& operator=(ScissorResult&& other)
  {
    if (this != &other)
    {
      CopyInputs(other);
      rectangles = std::move(other.rectangles);
    }
    return *this;
  }

  // The inputs, kept for the statistics overlay.
  ScissorPos scissor_tl;
  ScissorPos scissor_br;
  ScissorOffset scissor_off;
  float viewport_left;
  float viewport_right;
  float viewport_top;
  float viewport_bottom;

  // Candidates, best last.
  std::vector<ScissorRect> rectangles;

  ScissorRect Best() const;

  bool ScissorMatches(const ScissorResult& other) const
  {
    return scissor_tl.hex == other.scissor_tl.hex && scissor_br.hex == other.scissor_br.hex &&
           scissor_off.hex == other.scissor_off.hex;
  }
  bool ViewportMatches(const ScissorResult& other) const
  {
    return viewport_left == other.viewport_left && viewport_right == other.viewport_right &&
           viewport_top == other.viewport_top && viewport_bottom == other.viewport_bottom;
  }
  bool Matches(const ScissorResult& other, bool compare_scissor, bool compare_viewport) const
  {
    return (!compare_scissor || ScissorMatches(other)) &&
           (!compare_viewport || ViewportMatches(other));
  }

private:
  ScissorResult(ScissorPos scissor_top_left, ScissorPos scissor_bottom_right,
                ScissorOffset scissor_offset, std::pair<float, float> viewport_x,
                std::pair<float, float> viewport_y);

  void CopyInputs(const ScissorResult& other)
  {
    scissor_tl.hex = other.scissor_tl.hex;
    scissor_br.hex = other.scissor_br.hex;
    scissor_off.hex = other.scissor_off.hex;
    viewport_left = other.viewport_left;
    viewport_right = other.viewport_right;
    viewport_top = other.viewport_top;
    viewport_bottom = other.viewport_bottom;
  }

  int GetViewportArea(const ScissorRect& rect) const;
  bool IsWorse(const ScissorRect& lhs, const ScissorRect& rhs) const;
};

ScissorResult ComputeScissorRects(ScissorPos scissor_top_left, ScissorPos scissor_bottom_right,
                                  ScissorOffset scissor_offset, const Viewport& viewport);

void FlushPipeline();
void SetGenerationMode();
void SetScissorAndViewport(FramebufferManager* frame_buffer_manager, ScissorPos scissor_top_left,
                           ScissorPos scissor_bottom_right, ScissorOffset scissor_offset,
                           Viewport viewport);
void SetDepthMode();
void SetBlendMode();

// Clears the EFB if any channel is enabled; returns whether it did.
bool ClearScreen(FramebufferManager* frame_buffer_manager, const MathUtil::Rectangle<int>& rc,
                 bool color_enable, bool alpha_enable, bool z_enable, PixelFormat pixel_format,
                 u32 clear_color_ar, u32 clear_color_gb, u32 clear_z_value);

void OnPixelFormatChange(FramebufferManager* frame_buffer_manager, PixelFormat pixel_format,
                         DepthFormat z_format);
void SetInterlacingMode(const BPCmd& bp);
}  // namespace BPFunctions
