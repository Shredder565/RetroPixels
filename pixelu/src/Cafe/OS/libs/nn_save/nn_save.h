// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/OS/RPL/COSModule.h"

// nn_save: per-account save data access for the running title (SAVE* exports).
namespace nn::save
{
	// Unmounts /vol/save and forgets the cached account list (title stop).
	void ResetToDefaultState();

	// Persistent id of a save account slot (1..12, or 0xFF for the shared
	// "common" folder, which reports id 0). False when the slot is empty.
	bool GetPersistentIdEx(uint8 accountSlot, uint32* persistentId);

	COSModule* GetModule();
}
