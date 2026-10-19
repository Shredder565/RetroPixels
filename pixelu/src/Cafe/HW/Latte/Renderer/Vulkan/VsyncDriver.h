// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Host vertical blank source used when the game is paced by the display (Windows only).
// The callback runs on a dedicated thread once per vblank of the monitor that shows the main window.
void VsyncDriver_startThread(void(*cbVSync)());
// tells the vblank thread to check whether the main window moved to another monitor
void VsyncDriver_notifyWindowPosChanged();
