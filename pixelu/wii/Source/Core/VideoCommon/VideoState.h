// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

class PointerWrap;

// Saves or restores every piece of video emulation state, in the fixed savestate order.
void VideoCommon_DoState(PointerWrap& p);
