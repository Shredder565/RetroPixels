// Original code for RetroPixels (PixelU). Not derived from Cemu.
// erreula: the system error viewer.
#pragma once
#include "Cafe/OS/RPL/COSModule.h"

namespace nn::erreula
{
	// draws the dialog, if one is up; called by the renderer every frame
	void render(bool mainWindow);

	COSModule* GetModule();
}
