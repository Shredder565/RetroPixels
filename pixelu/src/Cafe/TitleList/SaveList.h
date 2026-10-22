// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "SaveInfo.h"

struct CafeSaveListCallbackEvent
{
	enum class TYPE
	{
		SAVE_DISCOVERED,
		SAVE_REMOVED,
		SCAN_FINISHED,
	};
	TYPE eventType;
	SaveInfo* saveInfo; // null for SCAN_FINISHED
};

// Saves found in mlc/usr/save (only those with a readable meta/meta.xml)
class CafeSaveList
{
  public:
	static void Initialize();
	static void SetMLCPath(fs::path mlcPath);
	static void Refresh(); // rescans in the background (ignored while a scan runs)

	static SaveInfo GetSaveByTitleId(TitleId titleId);

	// the callback is immediately called for every known save (and SCAN_FINISHED if idle)
	static uint64 RegisterCallback(void (*cb)(CafeSaveListCallbackEvent* evt, void* ctx), void* ctx);
	static void UnregisterCallback(uint64 id);

  private:
	static void RefreshThreadWorker();
	static void DiscoveredSave(SaveInfo* saveInfo);
};
