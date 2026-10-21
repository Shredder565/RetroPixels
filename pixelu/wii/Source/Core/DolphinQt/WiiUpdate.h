// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

class QWidget;

// Updates the emulated Wii's system software, either from Nintendo's update servers or from the
// update partition of a disc image, with a progress dialog and a result message.
namespace WiiUpdate
{
void PerformOnlineUpdate(const std::string& region, QWidget* parent = nullptr);
void PerformDiscUpdate(const std::string& file_path, QWidget* parent = nullptr);
}  // namespace WiiUpdate
