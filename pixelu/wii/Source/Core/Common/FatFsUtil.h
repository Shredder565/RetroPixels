// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>

#include "Common/Common.h"
#include "Common/CommonTypes.h"

namespace Common
{
static constexpr auto SD_PACK_TEXT = _trans("Pack SD Card Now");
static constexpr auto SD_UNPACK_TEXT = _trans("Unpack SD Card Now");

// Builds the SD card image from the SD sync folder (replacing the image atomically). With
// `deterministic`, entries are written in name order and all timestamps are zero, so the same
// folder always gives the same image.
bool SyncSDFolderToSDImage(const std::function<bool()>& cancelled, bool deterministic);
// Replaces the SD sync folder with the contents of the SD card image (restoring the old folder
// on failure).
bool SyncSDImageToSDFolder(const std::function<bool()>& cancelled);

// The block device and clock behind FatFs while RunInFatFsContext() runs. Return values are
// FatFs DSTATUS / DRESULT codes.
class FatFsCallbacks
{
public:
  FatFsCallbacks();
  virtual ~FatFsCallbacks();

  virtual u8 DiskInitialize(u8 pdrv);
  virtual u8 DiskStatus(u8 pdrv);
  virtual int DiskRead(u8 pdrv, u8* buff, u32 sector, unsigned int count) = 0;
  virtual int DiskWrite(u8 pdrv, const u8* buff, u32 sector, unsigned int count) = 0;
  virtual int DiskIOCtl(u8 pdrv, u8 cmd, void* buff) = 0;
  // Current local time in FAT's packed date/time format.
  virtual u32 GetCurrentTimeFAT();
};

// Runs `function` with FatFs bound to `callbacks`. FatFs is global, so callers are serialised.
void RunInFatFsContext(FatFsCallbacks& callbacks, const std::function<void()>& function);
}  // namespace Common
