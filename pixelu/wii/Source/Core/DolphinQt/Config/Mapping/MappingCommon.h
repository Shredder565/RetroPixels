// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <chrono>

class MappingWindow;

namespace MappingCommon
{
// Wait this long after a detection is requested before listening for input, so the key press
// that "clicked" the button is not picked up as the new mapping.
static constexpr auto INPUT_DETECT_INITIAL_DELAY = std::chrono::milliseconds{100};

// Attaches the input detection queue of a mapping window to it (owned by the window).
void CreateMappingProcessor(MappingWindow*);
}  // namespace MappingCommon
