// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Widescreen.h"

#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/System.h"

#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexManagerBase.h"

std::unique_ptr<WidescreenManager> g_widescreen;

namespace
{
// Forced aspect modes map to a fixed answer; other modes give none.
std::optional<bool> ForcedWidescreen(AspectMode mode)
{
  if (mode == AspectMode::ForceStandard)
    return false;
  if (mode == AspectMode::ForceWide)
    return true;
  return std::nullopt;
}
}  // namespace

WidescreenManager::WidescreenManager()
{
  if (const std::optional<bool> forced = GetWidescreenOverride())
    m_is_game_widescreen = *forced;

  const AspectMode suggested = g_ActiveConfig.suggested_aspect_mode;
  if (suggested != AspectMode::Auto && suggested != AspectMode::ForceStandard &&
      suggested != AspectMode::ForceWide)
  {
    // Other modes (stretch, custom) mean nothing as a game suggestion.
    WARN_LOG_FMT(VIDEO,
                 "Invalid suggested aspect ratio mode: only Auto, 4:3 and 16:9 are supported");
  }

  auto& system = Core::System::GetInstance();
  auto& events = system.GetVideoEvents();
  m_config_hook =
      events.config_changed_event.Register([this](u32 bits) { OnConfigChanged(bits); });

  // The projection statistics the heuristic needs are only gathered for GameCube games.
  if (!system.IsWii())
  {
    m_after_frame_hook =
        events.after_frame_event.Register([this](Core::System&) { UpdateWidescreenHeuristic(); });
  }
}

void WidescreenManager::OnConfigChanged(u32 changed_bits)
{
  if ((changed_bits & CONFIG_CHANGE_BIT_ASPECT_RATIO) == 0)
    return;

  // Settings win; without them fall back to what the heuristic last saw (or 4:3).
  if (const std::optional<bool> forced = GetWidescreenOverride())
    m_is_game_widescreen = *forced;
  else
    m_is_game_widescreen = m_detection == Detection::SawAnamorphic;
}

std::optional<bool> WidescreenManager::GetWidescreenOverride() const
{
  std::optional<bool> result;

  if (Core::System::GetInstance().IsWii())
    result = Config::Get(Config::SYSCONF_WIDESCREEN);

  // A game INI suggestion beats the Wii system setting.
  if (const std::optional<bool> suggested = ForcedWidescreen(g_ActiveConfig.suggested_aspect_mode))
    result = suggested;

  // Without the widescreen hack, a forced 4:3 or 16:9 display mode beats both.
  if (!g_ActiveConfig.bWidescreenHack)
  {
    if (const std::optional<bool> display = ForcedWidescreen(g_ActiveConfig.aspect_mode))
      result = display;
  }

  return result;
}

// GameCube anamorphic detection: the vertex manager counts vertices drawn with projections
// whose aspect matches the viewport ("normal") or a horizontally squeezed one ("anamorphic").
// Only 4:3 and 16:9 are recognised.
void WidescreenManager::UpdateWidescreenHeuristic()
{
  const auto stats = g_vertex_manager->ResetFlushAspectRatioCount();
  const bool ortho_was_anamorphic = m_last_ortho_anamorphic;
  m_detection = Detection::Off;
  m_last_ortho_anamorphic = false;

  // A game INI suggestion turns detection off (and then the override cannot have changed).
  if (g_ActiveConfig.suggested_aspect_mode != AspectMode::Auto)
    return;

  std::optional<bool> result = GetWidescreenOverride();

  const AspectMode display = g_ActiveConfig.aspect_mode;
  const bool display_forced =
      display == AspectMode::ForceStandard || display == AspectMode::ForceWide;
  if (g_ActiveConfig.bWidescreenHack || !display_forced)
  {
    // One kind has to outnumber the other by the threshold factor, which keeps the result from
    // flipping on a few stray draws.
    const u32 factor = g_ActiveConfig.widescreen_heuristic_transition_threshold;
    const auto dominated_by_normal = [factor](const auto& c) {
      return c.normal_vertex_count > c.anamorphic_vertex_count * factor;
    };
    const auto dominated_by_anamorphic = [factor](const auto& c) {
      return c.anamorphic_vertex_count > c.normal_vertex_count * factor;
    };

    const auto& persp = stats.perspective;
    const auto& ortho = stats.orthographic;
    g_stats.avg_persp_proj_viewport_ratio = persp.average_ratio.Mean();
    g_stats.avg_ortho_proj_viewport_ratio = ortho.average_ratio.Mean();

    const bool ortho_anamorphic = dominated_by_anamorphic(ortho);
    const bool persp_normal = dominated_by_normal(persp);

    if (ortho_anamorphic || dominated_by_anamorphic(persp))
    {
      result = true;
      m_detection = Detection::SawAnamorphic;
    }
    else if (persp_normal || dominated_by_normal(ortho))
    {
      // Widescreen games often draw a 4:3-looking 2D layer over anamorphic 3D, so a normal
      // orthographic projection alone (say, a full-screen menu) only switches back to 4:3 when
      // the 2D layer itself had been anamorphic.
      if (persp_normal || ortho_was_anamorphic)
        result = false;
      m_detection = Detection::SawStandard;
    }
    else
    {
      m_detection = Detection::Undecided;
    }

    m_last_ortho_anamorphic = ortho_anamorphic;
  }

  if (result)
    m_is_game_widescreen = *result;
}

void WidescreenManager::DoState(PointerWrap& p)
{
  p.Do(m_is_game_widescreen);

  // Detection starts over after a load.
  if (p.IsReadMode())
  {
    m_last_ortho_anamorphic = false;
    m_detection = Detection::Off;
  }
}
