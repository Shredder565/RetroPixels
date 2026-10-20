// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Present.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

#include "Common/ChunkFile.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/CoreTiming.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Host.h"
#include "Core/System.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/OnScreenUI.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/Widescreen.h"

std::unique_ptr<VideoCommon::Presenter> g_presenter;

namespace VideoCommon
{
namespace
{
constexpr float ASPECT_4_3 = 4.0f / 3.0f;
constexpr float ASPECT_16_9 = 16.0f / 9.0f;

constexpr Presenter::ClearColor OPAQUE_BLACK = {0.0f, 0.0f, 0.0f, 1.0f};

// The ~16:9 equivalent of a ~4:3 analog picture (keeps the VI's small deviation from 4:3).
float WidenSourceAspect(float source_aspect)
{
  return source_aspect * (ASPECT_16_9 / ASPECT_4_3);
}

// "Stretch" depends on the window, which must not feed back into window sizing or dumps.
AspectMode EffectiveAspectMode(bool allow_stretch)
{
  const AspectMode mode = g_ActiveConfig.aspect_mode;
  return (!allow_stretch && mode == AspectMode::Stretch) ? AspectMode::Auto : mode;
}

// Of the four floor/ceil roundings of width x height, the one whose aspect ratio is nearest
// aspect_ratio (the first one found wins a tie).
std::tuple<int, int> FindClosestIntegerResolution(float width, float height, float aspect_ratio)
{
  const std::array<int, 2> widths = {static_cast<int>(std::ceil(width)),
                                     static_cast<int>(std::floor(width))};
  const std::array<int, 2> heights = {static_cast<int>(std::ceil(height)),
                                      static_cast<int>(std::floor(height))};

  std::tuple<int, int> best(widths[1], heights[1]);
  float best_error = std::numeric_limits<float>::max();
  for (const int w : widths)
  {
    for (const int h : heights)
    {
      const float error = std::abs(static_cast<float>(w) / static_cast<float>(h) / aspect_ratio - 1.f);
      if (error < best_error)
      {
        best_error = error;
        best = {w, h};
      }
    }
  }
  return best;
}

// When the output is within one (scaled) pixel of the XFB size in one direction and exact in
// the other, use the XFB size so the picture is not resampled for nothing. The VI aspect ratio
// and the various roundings make such near misses common.
void TryToSnapToXFBSize(int& width, int& height, int xfb_width, int xfb_height)
{
  // Nothing to snap to while the screen is blanked.
  if (xfb_width == 0 || xfb_height == 0)
    return;

  const unsigned int tolerance = g_framebuffer_manager->GetEFBScale();
  const auto off_x = static_cast<unsigned int>(std::abs(width - xfb_width));
  const auto off_y = static_cast<unsigned int>(std::abs(height - xfb_height));
  const bool near_horizontally = off_x <= tolerance && off_y == 0;
  const bool near_vertically = off_y <= tolerance && off_x == 0;
  if (near_horizontally || near_vertically)
  {
    width = xfb_width;
    height = xfb_height;
  }
}

// Rounds a frame dump dimension up to a positive multiple of what the encoder needs.
int PadToMultiple(int size, int multiple)
{
  const int remainder = size % multiple;
  if (remainder != 0 || size == 0)
    size += multiple - remainder;
  return size;
}
}  // namespace

Presenter::Presenter()
{
  VideoEvents& events = GetVideoEvents();
  m_config_changed =
      events.config_changed_event.Register([this](u32 bits) { ConfigChanged(bits); });
  m_end_field_hook = events.vi_end_field_event.Register(
      [this] { m_immediate_swap_happened_this_field.store(false, std::memory_order_relaxed); });
}

Presenter::~Presenter()
{
  // Mouse mapping in the controller dialogs should see the whole window again.
  g_controller_interface.SetAspectRatioAdjustment(1);
}

bool Presenter::Initialize()
{
  UpdateDrawRectangle();
  m_immediate_swap_happened_this_field.store(false, std::memory_order_relaxed);

  if (g_gfx->IsHeadless())
    return true;

  SetBackbuffer(g_gfx->GetSurfaceInfo());

  m_post_processor = std::make_unique<VideoCommon::PostProcessing>();
  if (!m_post_processor->Initialize(m_backbuffer_format))
    return false;

  m_onscreen_ui = std::make_unique<OnScreenUI>();
  if (!m_onscreen_ui->Initialize(static_cast<u32>(m_backbuffer_width),
                                 static_cast<u32>(m_backbuffer_height), m_backbuffer_scale))
    return false;

  // One black frame, which also finishes setting up the UI.
  g_gfx->BindBackbuffer(OPAQUE_BLACK);
  g_gfx->PresentBackbuffer();
  return true;
}

bool Presenter::FetchXFB(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height, u64 ticks)
{
  ReleaseXFBContentLock();
  const u64 previous_id = m_last_xfb_id;

  const bool blanked = fb_width == 0 || fb_height == 0;
  if (blanked)
  {
    m_xfb_entry.reset();
    m_xfb_rect = MathUtil::Rectangle<int>();
    m_last_xfb_id = std::numeric_limits<u64>::max();
  }
  else
  {
    m_xfb_entry =
        g_texture_cache->GetXFBTexture(xfb_addr, fb_width, fb_height, fb_stride, &m_xfb_rect);
    m_last_xfb_id = m_xfb_entry->id;
    m_xfb_entry->AcquireContentLock();
  }

  m_last_xfb_addr = xfb_addr;
  m_last_xfb_ticks = ticks;
  m_last_xfb_width = fb_width;
  m_last_xfb_stride = fb_stride;
  m_last_xfb_height = fb_height;
  return m_last_xfb_id == previous_id;
}

void Presenter::ViSwap(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height, u64 ticks,
                       TimePoint presentation_time)
{
  const bool repeat = FetchXFB(xfb_addr, fb_width, fb_stride, fb_height, ticks);

  PresentInfo info{
      .present_count = m_present_count++,
      .emulated_timestamp = ticks,
      .intended_present_time = presentation_time,
      .frame_buffer_width = fb_width,
      .frame_buffer_height = fb_height,
      .xfb_copy_hashes = {},
  };
  if (repeat)
  {
    // The same frame number as the last one.
    info.frame_count = m_frame_count - 1;
    info.reason = PresentInfo::PresentReason::VideoInterfaceDuplicate;
  }
  else
  {
    info.frame_count = m_frame_count++;
    info.reason = PresentInfo::PresentReason::VideoInterface;
  }

  // The texture names of the EFB copies the XFB is made of (itself, when not stitched).
  if (m_xfb_entry)
  {
    const auto add_name = [&info](const TCacheEntry& entry) {
      if (!entry.texture_info_name.empty())
        info.xfb_copy_hashes.push_back(entry.texture_info_name);
    };
    if (m_xfb_entry->references.empty())
    {
      add_name(*m_xfb_entry);
    }
    else
    {
      for (const auto& part : m_xfb_entry->references)
        add_name(*part);
    }
  }

  VideoEvents& events = GetVideoEvents();
  events.before_present_event.Trigger(info);
  if (repeat && g_ActiveConfig.bSkipPresentingDuplicateXFBs)
    return;

  Present(&info);
  ProcessFrameDumping(ticks);
  events.after_present_event.Trigger(info);
}

void Presenter::ImmediateSwap(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height)
{
  // With the cap on, only the first immediate swap of each VI field is shown.
  const bool already_swapped =
      m_immediate_swap_happened_this_field.exchange(true, std::memory_order_relaxed);
  if (already_swapped && Config::Get(Config::GFX_HACK_CAP_IMMEDIATE_XFB))
    return;

  const u64 ticks = m_next_swap_estimated_ticks;
  FetchXFB(xfb_addr, fb_width, fb_stride, fb_height, ticks);

  PresentInfo info{
      .frame_count = m_frame_count++,
      .present_count = m_present_count++,
      .emulated_timestamp = ticks,
      .intended_present_time = m_next_swap_estimated_time,
      .reason = PresentInfo::PresentReason::Immediate,
      .frame_buffer_width = fb_width,
      .frame_buffer_height = fb_height,
      .xfb_copy_hashes = {},
  };

  VideoEvents& events = GetVideoEvents();
  events.before_present_event.Trigger(info);
  Present(&info);
  ProcessFrameDumping(ticks);
  events.after_present_event.Trigger(info);
}

void Presenter::SetNextSwapEstimatedTime(u64 ticks, TimePoint host_time)
{
  m_next_swap_estimated_ticks = ticks;
  m_next_swap_estimated_time = host_time;
}

void Presenter::ProcessFrameDumping(u64 ticks) const
{
  if (!g_frame_dumper->IsFrameDumping() || !m_xfb_entry)
    return;

  // The size of the dumped image. Black bars are never included.
  const auto aspect_corrected_size = [this]() -> std::tuple<int, int> {
    constexpr bool allow_stretch = false;
    const auto [w, h] =
        ScaleToDisplayAspectRatio(m_xfb_rect.GetWidth(), m_xfb_rect.GetHeight(), allow_stretch);
    return FindClosestIntegerResolution(w, h, CalculateDrawAspectRatio(allow_stretch));
  };

  int width;
  int height;
  switch (Config::Get(Config::GFX_FRAME_DUMPS_RESOLUTION_TYPE))
  {
  case FrameDumpResolutionType::XFBRawResolution:
    width = m_xfb_rect.GetWidth();
    height = m_xfb_rect.GetHeight();
    break;
  case FrameDumpResolutionType::XFBAspectRatioCorrectedResolution:
    std::tie(width, height) = aspect_corrected_size();
    break;
  case FrameDumpResolutionType::WindowResolution:
  default:
    // Without a window, fall back to the aspect-corrected XFB size.
    if (g_gfx->IsHeadless())
    {
      std::tie(width, height) = aspect_corrected_size();
    }
    else
    {
      width = GetTargetRectangle().GetWidth();
      height = GetTargetRectangle().GetHeight();
    }
    break;
  }

  // Encoders need sizes divisible by some number (and non-zero). Rounding up loses nothing.
  // Screenshots would not need it, and the resampling done here is not gamma correct.
  const int multiple = g_frame_dumper->GetRequiredResolutionLeastCommonMultiple();
  const MathUtil::Rectangle<int> output(0, 0, PadToMultiple(width, multiple),
                                        PadToMultiple(height, multiple));
  g_frame_dumper->DumpCurrentFrame(m_xfb_entry->texture.get(), m_xfb_rect, output, ticks,
                                   static_cast<int>(m_frame_count));
}

void Presenter::SetBackbuffer(int backbuffer_width, int backbuffer_height)
{
  const bool first = m_backbuffer_width == 0 && m_backbuffer_height == 0;
  const bool resized =
      backbuffer_width != m_backbuffer_width || backbuffer_height != m_backbuffer_height;
  m_backbuffer_width = backbuffer_width;
  m_backbuffer_height = backbuffer_height;
  UpdateDrawRectangle();
  OnBackbufferSet(resized, first);
}

void Presenter::SetBackbuffer(SurfaceInfo info)
{
  const int width = static_cast<int>(info.width);
  const int height = static_cast<int>(info.height);
  const bool first = m_backbuffer_width == 0 && m_backbuffer_height == 0;
  const bool resized = width != m_backbuffer_width || height != m_backbuffer_height;
  m_backbuffer_width = width;
  m_backbuffer_height = height;
  m_backbuffer_scale = info.scale;
  m_backbuffer_format = info.format;
  if (m_onscreen_ui)
    m_onscreen_ui->SetScale(info.scale);
  OnBackbufferSet(resized, first);
}

void Presenter::OnBackbufferSet(bool size_changed, bool is_first_set)
{
  UpdateDrawRectangle();

  // "Auto (window size)" internal resolution follows the window.
  const bool follow_window = size_changed && !is_first_set &&
                             g_ActiveConfig.iEFBScale == EFB_SCALE_AUTO_INTEGRAL &&
                             m_auto_resolution_scale != AutoIntegralScale();
  if (follow_window)
    g_framebuffer_manager->RecreateEFBFramebuffer(g_ActiveConfig.iEFBScale);

  if (size_changed || is_first_set)
    m_auto_resolution_scale = AutoIntegralScale();
}

void Presenter::ConfigChanged(u32 changed_bits)
{
  // A different post-processing shader. (Its options are read every frame.)
  if ((changed_bits & ConfigChangeBits::CONFIG_CHANGE_BIT_POST_PROCESSING_SHADER) != 0 &&
      m_post_processor)
  {
    // The GPU may still be using the old one.
    g_gfx->WaitForGPUIdle();
    m_post_processor->RecompileShader();
  }

  // Stereo layout is baked into the UI and post-processing pipelines.
  if ((changed_bits & ConfigChangeBits::CONFIG_CHANGE_BIT_STEREO_MODE) != 0)
  {
    if (m_onscreen_ui)
      m_onscreen_ui->RecompileImGuiPipeline();
    if (m_post_processor)
      m_post_processor->RecompilePipeline();
  }
}

std::tuple<MathUtil::Rectangle<int>, MathUtil::Rectangle<int>>
Presenter::ConvertStereoRectangle(const MathUtil::Rectangle<int>& rc) const
{
  const bool vertical = g_ActiveConfig.stereo_mode == StereoMode::TopAndBottom;

  // Halve the rectangle about its centre (the extent may be negative for flipped rectangles),
  // then move one copy towards each half of the backbuffer.
  MathUtil::Rectangle<int> eye = rc;
  if (vertical)
  {
    const int quarter = (rc.bottom - rc.top) / 4;
    eye.top += quarter;
    eye.bottom -= quarter;
  }
  else
  {
    const int quarter = (rc.right - rc.left) / 4;
    eye.left += quarter;
    eye.right -= quarter;
  }

  MathUtil::Rectangle<int> left = eye;
  MathUtil::Rectangle<int> right = eye;
  if (vertical)
  {
    const int shift = m_backbuffer_height / 4;
    left.top -= shift;
    left.bottom -= shift;
    right.top += shift;
    right.bottom += shift;
  }
  else
  {
    const int shift = m_backbuffer_width / 4;
    left.left -= shift;
    left.right -= shift;
    right.left += shift;
    right.right += shift;
  }
  return {left, right};
}

MathUtil::Rectangle<int> Presenter::GetCustomCrop(const MathUtil::Rectangle<int>& rect) const
{
  if (!g_ActiveConfig.bCropCustom)
    return MathUtil::Rectangle<int>(0, 0, 0, 0);

  // Configured in native pixels; never more than what is left of the picture.
  const int scale = static_cast<int>(g_framebuffer_manager->GetEFBScale());
  const int width = rect.GetWidth();
  const int height = rect.GetHeight();
  const int left = std::min(g_ActiveConfig.iCropCustomLeft * scale, width);
  const int right = std::min(g_ActiveConfig.iCropCustomRight * scale, width - left);
  const int top = std::min(g_ActiveConfig.iCropCustomTop * scale, height);
  const int bottom = std::min(g_ActiveConfig.iCropCustomBottom * scale, height - top);
  return MathUtil::Rectangle<int>(left, top, right, bottom);
}

MathUtil::Rectangle<int> Presenter::AdjustForCustomCrop(const MathUtil::Rectangle<int>& rect) const
{
  const MathUtil::Rectangle<int> amount = GetCustomCrop(rect);
  return MathUtil::Rectangle<int>(rect.left + amount.left, rect.top + amount.top,
                                  rect.right - amount.right, rect.bottom - amount.bottom);
}

float Presenter::AdjustAspectRatioForCustomCrop(float input_aspect_ratio) const
{
  if (!g_ActiveConfig.bCropCustom)
    return input_aspect_ratio;

  const MathUtil::Rectangle<int> full = m_xfb_rect;
  if (full.GetWidth() <= 0 || full.GetHeight() <= 0)
    return input_aspect_ratio;

  const MathUtil::Rectangle<int> kept = AdjustForCustomCrop(full);
  const float width_kept = static_cast<float>(kept.GetWidth()) / static_cast<float>(full.GetWidth());
  const float height_kept =
      static_cast<float>(kept.GetHeight()) / static_cast<float>(full.GetHeight());
  return input_aspect_ratio * (width_kept / height_kept);
}

float Presenter::CalculateDrawAspectRatio(bool allow_stretch) const
{
  const AspectMode mode = EffectiveAspectMode(allow_stretch);

  float aspect;
  if (mode == AspectMode::Stretch)
  {
    // Fill the window.
    aspect = static_cast<float>(m_backbuffer_width) / static_cast<float>(m_backbuffer_height);
  }
  else
  {
    // The VI decides the shape of the picture, not the XFB texture.
    const float vi_aspect = Core::System::GetInstance().GetVideoInterface().GetAspectRatio();
    const float source = AdjustAspectRatioForCustomCrop(vi_aspect);
    switch (mode)
    {
    case AspectMode::Auto:
      aspect = g_widescreen->IsGameWidescreen() ? WidenSourceAspect(source) : source;
      break;
    case AspectMode::ForceWide:
      aspect = WidenSourceAspect(source);
      break;
    case AspectMode::Custom:
      // Relative to 4:3, so the VI's deviation from 4:3 carries over.
      aspect = source * (g_ActiveConfig.GetCustomAspectRatio() / ASPECT_4_3);
      break;
    case AspectMode::CustomStretch:
      // Exactly the configured ratio.
      aspect = g_ActiveConfig.GetCustomAspectRatio();
      break;
    case AspectMode::Raw:
      aspect = m_xfb_entry ?
                   static_cast<float>(m_last_xfb_width) / static_cast<float>(m_last_xfb_height) :
                   1.f;
      break;
    default:
      aspect = source;
      break;
    }
  }

  // Full-resolution-per-eye stereo is later squeezed into half the backbuffer.
  if (g_ActiveConfig.stereo_per_eye_resolution_full)
  {
    if (g_ActiveConfig.stereo_mode == StereoMode::SideBySide)
      aspect *= 2.0f;
    else if (g_ActiveConfig.stereo_mode == StereoMode::TopAndBottom)
      aspect /= 2.0f;
  }
  return aspect;
}

void Presenter::AdjustRectanglesToFitBounds(MathUtil::Rectangle<int>* target_rect,
                                            MathUtil::Rectangle<int>* source_rect, int fb_width,
                                            int fb_height)
{
  // Ratios from the rectangles as passed in.
  const int target_w = target_rect->GetWidth();
  const int target_h = target_rect->GetHeight();
  const int source_w = source_rect->GetWidth();
  const int source_h = source_rect->GetHeight();
  const auto to_source_x = [&](int target_pixels) { return target_pixels * source_w / target_w; };
  const auto to_source_y = [&](int target_pixels) { return target_pixels * source_h / target_h; };

  if (target_rect->left < 0)
  {
    source_rect->left += to_source_x(-target_rect->left);
    target_rect->left = 0;
  }
  if (target_rect->right > fb_width)
  {
    const int excess = target_rect->right - fb_width;
    source_rect->right -= to_source_x(excess);
    target_rect->right -= excess;
  }
  if (target_rect->top < 0)
  {
    source_rect->top += to_source_y(-target_rect->top);
    target_rect->top = 0;
  }
  if (target_rect->bottom > fb_height)
  {
    const int excess = target_rect->bottom - fb_height;
    source_rect->bottom -= to_source_y(excess);
    target_rect->bottom -= excess;
  }
}

void Presenter::ReleaseXFBContentLock()
{
  if (m_xfb_entry != nullptr)
    m_xfb_entry->ReleaseContentLock();
}

void Presenter::ChangeSurface(void* new_surface_handle)
{
  std::lock_guard guard(m_swap_mutex);
  m_new_surface_handle = new_surface_handle;
  m_surface_changed.Set();
}

void Presenter::ResizeSurface()
{
  std::lock_guard guard(m_swap_mutex);
  m_surface_resized.Set();
}

void* Presenter::GetNewSurfaceHandle()
{
  return std::exchange(m_new_surface_handle, nullptr);
}

u32 Presenter::AutoIntegralScale() const
{
  // The smallest integer multiple of the XFB (shaped like the window's draw area) that is at
  // least as large as that area in both directions, capped by the maximum EFB scale. This and
  // the "auto-size window" setting can influence each other.
  const u32 target_width = static_cast<u32>(m_target_rectangle.GetWidth());
  const u32 target_height = static_cast<u32>(m_target_rectangle.GetHeight());
  const float target_aspect =
      static_cast<float>(target_width) / static_cast<float>(target_height);

  u32 width = m_last_xfb_width;
  u32 height = m_last_xfb_height;
  const float xfb_aspect = static_cast<float>(width) / static_cast<float>(height);
  if (xfb_aspect >= target_aspect)
    width = static_cast<u32>(std::round(static_cast<float>(height) * target_aspect));
  else
    height = static_cast<u32>(std::round(static_cast<float>(width) / target_aspect));

  const auto multiple_to_cover = [](u32 target, u32 size) -> u32 {
    return size == 0 ? 1 : (target + size - 1) / size;
  };
  const u32 scale =
      std::max(multiple_to_cover(target_width, width), multiple_to_cover(target_height, height));
  return std::min(scale, static_cast<u32>(Config::Get(Config::GFX_MAX_EFB_SCALE)));
}

void Presenter::SetSuggestedWindowSize(int width, int height)
{
  const auto [request_width, request_height] =
      CalculateOutputDimensions(width, height, /*allow_stretch=*/false);

  // Only ask again when the answer changes. The window system may ignore it anyway.
  if (request_width == m_last_window_request_width &&
      request_height == m_last_window_request_height)
  {
    return;
  }
  m_last_window_request_width = request_width;
  m_last_window_request_height = request_height;
  Host_RequestRenderWindowSize(request_width, request_height);
}

std::tuple<float, float> Presenter::ApplyStandardAspectCrop(float width, float height,
                                                            bool allow_stretch) const
{
  const AspectMode mode = EffectiveAspectMode(allow_stretch);
  if (!g_ActiveConfig.bCropToAspectRatio || mode == AspectMode::Stretch || mode == AspectMode::Raw)
    return {width, height};

  float wanted;
  switch (mode)
  {
  case AspectMode::ForceWide:
    wanted = ASPECT_16_9;
    break;
  case AspectMode::ForceStandard:
    wanted = ASPECT_4_3;
    break;
  case AspectMode::Custom:
  case AspectMode::CustomStretch:
    // Custom crops from the VI shape to the exact target; custom stretch should already match.
    wanted = g_ActiveConfig.GetCustomAspectRatio();
    break;
  case AspectMode::Auto:
  default:
    wanted = g_widescreen->IsGameWidescreen() ? ASPECT_16_9 : ASPECT_4_3;
    break;
  }

  // Trim whichever side is too long.
  if (width / height > wanted)
    width = height * wanted;
  else
    height = width / wanted;
  return {width, height};
}

void Presenter::UpdateDrawRectangle()
{
  const float draw_aspect = CalculateDrawAspectRatio();

  // Widescreen hack factors for the projection (used from the next frame on): the shape the
  // game renders for versus the shape it is shown at. Wider output widens the view (Hor+),
  // narrower output gives a taller view (Vert+).
  if (g_ActiveConfig.bWidescreenHack)
  {
    float game_aspect = Core::System::GetInstance().GetVideoInterface().GetAspectRatio();
    if (g_widescreen->IsGameWidescreen())
      game_aspect = WidenSourceAspect(game_aspect);

    const float ratio = game_aspect / draw_aspect;
    g_Config.fAspectRatioHackW = ratio > 1 ? 1 : ratio;
    g_Config.fAspectRatioHackH = ratio > 1 ? 1 / ratio : 1;
  }
  else
  {
    g_Config.fAspectRatioHackW = 1;
    g_Config.fAspectRatioHackH = 1;
  }

  const float window_w = static_cast<float>(m_backbuffer_width);
  const float window_h = static_cast<float>(m_backbuffer_height);
  const float window_aspect = window_w / window_h;

  // Lets mouse input map onto the drawn area. (Ignores cropping, and is unreliable for tiny
  // windows.)
  g_controller_interface.SetAspectRatioAdjustment(draw_aspect / window_aspect);

  // The picture as draw_aspect x 1, cropped to a standard shape if enabled, then scaled so the
  // cropped picture just fits the window.
  const auto [crop_w, crop_h] = ApplyStandardAspectCrop(draw_aspect, 1);
  const float fit = window_aspect >= crop_w / crop_h ? window_h / crop_h : window_w / crop_w;
  const float draw_w = draw_aspect * fit;
  const float draw_h = 1 * fit;

  int out_w;
  int out_h;
  if (g_ActiveConfig.aspect_mode == AspectMode::Raw && m_xfb_entry)
  {
    const MathUtil::Rectangle<int> xfb = AdjustForCustomCrop(m_xfb_rect);
    out_w = xfb.GetWidth();
    out_h = xfb.GetHeight();
  }
  else
  {
    // Whole pixels with the least aspect error (and so the least black bars).
    std::tie(out_w, out_h) = FindClosestIntegerResolution(draw_w, draw_h, draw_w / draw_h);
    if (!g_ActiveConfig.bCropToAspectRatio)
    {
      if (g_ActiveConfig.aspect_mode != AspectMode::Stretch)
      {
        const MathUtil::Rectangle<int> xfb = AdjustForCustomCrop(m_xfb_rect);
        TryToSnapToXFBSize(out_w, out_h, xfb.GetWidth(), xfb.GetHeight());
      }
      // Bigger than the window would just be cut off.
      out_w = std::min(out_w, static_cast<int>(window_w));
      out_h = std::min(out_h, static_cast<int>(window_h));
    }
  }

  // Centred.
  const double left = static_cast<double>(window_w) / 2.0 - out_w / 2.0;
  const double top = static_cast<double>(window_h) / 2.0 - out_h / 2.0;
  m_target_rectangle.left = static_cast<int>(std::round(left));
  m_target_rectangle.top = static_cast<int>(std::round(top));
  m_target_rectangle.right = m_target_rectangle.left + out_w;
  m_target_rectangle.bottom = m_target_rectangle.top + out_h;
}

std::tuple<float, float> Presenter::ScaleToDisplayAspectRatio(const int width, const int height,
                                                              bool allow_stretch) const
{
  // Only ever grow a side, so no resolution is lost.
  float w = static_cast<float>(width);
  float h = static_cast<float>(height);
  const float draw_aspect = CalculateDrawAspectRatio(allow_stretch);
  if (w / h >= draw_aspect)
    h = w / draw_aspect;
  else
    w = h * draw_aspect;
  return {w, h};
}

std::tuple<int, int> Presenter::CalculateOutputDimensions(int width, int height,
                                                          bool allow_stretch) const
{
  auto [w, h] = ScaleToDisplayAspectRatio(std::max(width, 1), std::max(height, 1), allow_stretch);
  std::tie(w, h) = ApplyStandardAspectCrop(w, h, allow_stretch);

  const AspectMode mode = EffectiveAspectMode(allow_stretch);
  if (g_ActiveConfig.bCropToAspectRatio || mode == AspectMode::Stretch)
    return {static_cast<int>(std::ceil(w)), static_cast<int>(std::ceil(h))};

  // Whole pixels at the right shape, so no thin black line appears at an edge.
  auto [out_w, out_h] = FindClosestIntegerResolution(w, h, CalculateDrawAspectRatio(allow_stretch));
  if (mode != AspectMode::Raw)
    TryToSnapToXFBSize(out_w, out_h, m_xfb_rect.GetWidth(), m_xfb_rect.GetHeight());
  return {out_w, out_h};
}

void Presenter::RenderXFBToScreen(const MathUtil::Rectangle<int>& target_rc,
                                  const AbstractTexture* source_texture,
                                  const MathUtil::Rectangle<int>& source_rc)
{
  const StereoMode stereo = g_ActiveConfig.stereo_mode;

  if (stereo == StereoMode::QuadBuffer && g_backend_info.bUsesExplictQuadBuffering)
  {
    // Each eye goes to its own back buffer.
    g_gfx->SelectLeftBuffer();
    m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture, 0);
    g_gfx->SelectRightBuffer();
    m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture, 1);
    g_gfx->SelectMainBuffer();
    return;
  }

  if (stereo == StereoMode::SideBySide || stereo == StereoMode::TopAndBottom)
  {
    const auto [left_eye, right_eye] = ConvertStereoRectangle(target_rc);
    m_post_processor->BlitFromTexture(left_eye, source_rc, source_texture, 0);
    m_post_processor->BlitFromTexture(right_eye, source_rc, source_texture, 1);
    return;
  }

  // Anything else copies all layers at once.
  m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture);
}

void Presenter::Present(PresentInfo* present_info)
{
  m_present_count++;

  if (g_gfx->IsHeadless() || (!m_onscreen_ui && !m_xfb_entry))
    return;

  // The window size follows the guest's XFB, so it is re-suggested after every frame.
  const auto suggest_window_size = [this] {
    const MathUtil::Rectangle<int> xfb = AdjustForCustomCrop(m_xfb_rect);
    SetSuggestedWindowSize(xfb.GetWidth(), xfb.GetHeight());
  };

  if (!g_gfx->SupportsUtilityDrawing())
  {
    // Backends without utility drawing (software) just show the XFB: no UI, no
    // post-processing.
    if (m_xfb_entry)
    {
      g_gfx->ShowImage(m_xfb_entry->texture.get(), AdjustForCustomCrop(m_xfb_rect));
      suggest_window_size();
    }
    return;
  }

  // Utility draws share the vertex buffer with the vertex loader; get its batch out first.
  g_vertex_manager->Flush();

  UpdateDrawRectangle();

  g_gfx->BeginUtilityDrawing();
  const bool have_backbuffer = g_gfx->BindBackbuffer(OPAQUE_BLACK);

  if (have_backbuffer && m_xfb_entry)
  {
    // Clip to the window by trimming the source, not with a viewport bigger than the window.
    MathUtil::Rectangle<int> target = GetTargetRectangle();
    MathUtil::Rectangle<int> source = AdjustForCustomCrop(m_xfb_rect);
    AdjustRectanglesToFitBounds(&target, &source, m_backbuffer_width, m_backbuffer_height);
    RenderXFBToScreen(target, m_xfb_entry->texture.get(), source);
  }

  if (m_onscreen_ui)
  {
    m_onscreen_ui->Finalize();
    if (have_backbuffer)
      m_onscreen_ui->DrawImGui();
  }

  {
    std::lock_guard<std::mutex> lock(m_swap_mutex);
    if (present_info)
    {
      const TimePoint when = GetUpdatedPresentationTime(present_info->intended_present_time);
      Core::System::GetInstance().GetCoreTiming().SleepUntil(when);
      // Only the hand-over time is known here.
      present_info->actual_present_time = Clock::now();
      present_info->present_time_accuracy = PresentInfo::PresentTimeAccuracy::PresentInProgress;
    }
    g_gfx->PresentBackbuffer();
  }

  if (m_xfb_entry)
    suggest_window_size();

  if (m_onscreen_ui)
    m_onscreen_ui->BeginImGuiFrame(static_cast<u32>(m_backbuffer_width),
                                   static_cast<u32>(m_backbuffer_height));

  g_gfx->EndUtilityDrawing();
}

TimePoint Presenter::GetUpdatedPresentationTime(TimePoint intended_presentation_time)
{
  const TimePoint now = Clock::now();
  // How early we are (zero or negative; being late counts as zero).
  const DT earliness = std::min(now - intended_presentation_time, DT{});

  if (!Config::Get(Config::MAIN_SMOOTH_EARLY_PRESENTATION))
  {
    m_presentation_time_offset = earliness;
    // For latency-focused setups, show it as soon as it is ready.
    const bool rush =
        g_ActiveConfig.bImmediateXFB || Config::Get(Config::MAIN_RUSH_FRAME_PRESENTATION);
    return rush ? now : intended_presentation_time;
  }

  // Follow the offset quickly when frames come later, slowly when they come earlier, so
  // occasional early frames do not make the pacing jumpy.
  const int smoothing = earliness < m_presentation_time_offset ? 100 : 2;
  m_presentation_time_offset += (earliness - m_presentation_time_offset) / smoothing;
  return intended_presentation_time + m_presentation_time_offset;
}

// Input for the UI is dropped while there is none (headless).
void Presenter::SetKeyMap(const DolphinKeyMap& key_map)
{
  if (OnScreenUI* const ui = m_onscreen_ui.get())
    ui->SetKeyMap(key_map);
}

void Presenter::SetKey(u32 key, bool is_down, const char* chars)
{
  if (OnScreenUI* const ui = m_onscreen_ui.get())
    ui->SetKey(key, is_down, chars);
}

void Presenter::SetMousePos(float x, float y)
{
  if (OnScreenUI* const ui = m_onscreen_ui.get())
    ui->SetMousePos(x, y);
}

void Presenter::SetMousePress(u32 button_mask)
{
  if (OnScreenUI* const ui = m_onscreen_ui.get())
    ui->SetMousePress(button_mask);
}

void Presenter::DoState(PointerWrap& p)
{
  // Frame counter and the last XFB the guest asked for.
  p.Do(m_frame_count);
  p.Do(m_last_xfb_ticks);
  p.Do(m_last_xfb_addr);
  p.Do(m_last_xfb_width);
  p.Do(m_last_xfb_stride);
  p.Do(m_last_xfb_height);

  if (!p.IsReadMode() || m_last_xfb_stride == 0)
    return;

  // After loading, show the saved XFB again. That ends a frame as far as listeners care.
  GetVideoEvents().after_frame_event.Trigger(Core::System::GetInstance());
  m_next_swap_estimated_ticks = m_last_xfb_ticks;
  m_next_swap_estimated_time = Clock::now();
  m_immediate_swap_happened_this_field.store(false, std::memory_order_relaxed);
  ImmediateSwap(m_last_xfb_addr, m_last_xfb_width, m_last_xfb_stride, m_last_xfb_height);
}

}  // namespace VideoCommon
