// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// On-screen overlay: statistics and notifications drawn with ImGui over the TV or GamePad image

void LatteOverlay_init();
void LatteOverlay_render(bool pad_view);
void LatteOverlay_updateStats(double fps, sint32 drawcalls, sint32 fastDrawcalls);

// shows `text` for `duration` milliseconds
void LatteOverlay_pushNotification(const std::string& text, sint32 duration);
