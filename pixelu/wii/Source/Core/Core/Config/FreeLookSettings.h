// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/Config/ConfigInfo.h"

namespace FreeLook
{
enum class ControlType : int
{
  SixAxis,
  FPS,
  Orbital
};
}

namespace Config
{
// Free look camera settings (FreeLook.ini).

extern const Info<bool> FREE_LOOK_ENABLED;
extern const Info<bool> FREE_LOOK_BACKGROUND_INPUT;

// [Camera1]
extern const Info<FreeLook::ControlType> FL1_CONTROL_TYPE;

}  // namespace Config
