// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include "Common/CommonPaths.h"

// Location of the user's graphics mods, relative to the user directory ("Load/GraphicsMods/").
static const inline std::string DOLPHIN_SYSTEM_GRAPHICS_MOD_DIR =
    LOAD_DIR DIR_SEP GRAPHICSMOD_DIR DIR_SEP;
