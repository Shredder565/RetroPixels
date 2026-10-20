// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Config/FreeLookSettings.h"

namespace Config
{
// Global switches, in [General] of FreeLook.ini.
const Info<bool> FREE_LOOK_ENABLED{Location{System::FreeLook, "General", "Enabled"}, false};
const Info<bool> FREE_LOOK_BACKGROUND_INPUT{
    Location{System::FreeLook, "General", "BackgroundInput"}, false};

// How the first (and only) free look camera is steered.
const Info<FreeLook::ControlType> FL1_CONTROL_TYPE{
    Location{System::FreeLook, "Camera1", "ControlType"}, FreeLook::ControlType::SixAxis};

}  // namespace Config
