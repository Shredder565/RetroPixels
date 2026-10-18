// Original code for RetroPixels (PixelU). Not derived from Cemu.
// nn_ac: network connection state.
#pragma once
#include "Cafe/OS/RPL/COSModule.h"

void nnAc_load();

namespace nn_ac
{
	// also used by nn_boss
	nnResult IsApplicationConnected(uint8be* connected);
}

namespace nn::ac
{
	COSModule* GetModule();
}
