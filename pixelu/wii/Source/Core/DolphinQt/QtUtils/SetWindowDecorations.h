// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#if defined(_WIN32)

class QApplication;

namespace QtUtils
{
// Gives every top-level window a dark title bar whenever the active theme is dark (Windows does
// not do this by itself for Qt windows).
void InstallWindowDecorationFilter(QApplication*);
}  // namespace QtUtils

#endif
