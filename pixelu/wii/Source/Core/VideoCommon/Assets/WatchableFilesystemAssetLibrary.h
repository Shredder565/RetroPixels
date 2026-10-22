// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/FilesystemWatcher.h"
#include "VideoCommon/Assets/CustomAssetLibrary.h"

namespace VideoCommon
{
// An asset library whose backing files are watched, so edits on disk can trigger reloads.
class WatchableFilesystemAssetLibrary : public CustomAssetLibrary, public Common::FilesystemWatcher
{
};
}  // namespace VideoCommon
