// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// Dear ImGui build configuration (IMGUI_USER_CONFIG), seen by imgui itself and by
// every file that includes imgui.h.

#pragma once

// ImGui's own checks report through PixelU-Wii's assertion handler.
#include "Common/Assert.h"
#define IM_ASSERT(condition) ASSERT(condition)

// The on-screen display never opens ImGui's demo window; leave its code out.
#define IMGUI_DISABLE_DEMO_WINDOWS
