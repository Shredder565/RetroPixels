// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <optional>

#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"

class PointerWrap;

// Decides whether the game is showing 4:3 or 16:9 (anamorphic) pictures. Settings can force
// either; otherwise Wii games follow the system setting and GameCube games are watched by a
// heuristic on their projection matrices.
class WidescreenManager
{
public:
  WidescreenManager();

  // True when the game is (or is being treated as) widescreen.
  bool IsGameWidescreen() const { return m_is_game_widescreen; }

  void DoState(PointerWrap& p);

private:
  enum class Detection
  {
    Off,
    Undecided,
    SawStandard,
    SawAnamorphic,
  };

  // The widescreen value settings impose, if any.
  std::optional<bool> GetWidescreenOverride() const;
  // Runs after each frame on GameCube.
  void UpdateWidescreenHeuristic();
  void OnConfigChanged(u32 changed_bits);

  bool m_is_game_widescreen = false;
  bool m_last_ortho_anamorphic = false;
  Detection m_detection = Detection::Off;

  Common::EventHook m_after_frame_hook;
  Common::EventHook m_config_hook;
};

extern std::unique_ptr<WidescreenManager> g_widescreen;
