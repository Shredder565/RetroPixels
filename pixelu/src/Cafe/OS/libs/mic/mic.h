// Original code for RetroPixels (PixelU). Not derived from Cemu.
// mic: GamePad microphone.
#pragma once
#include "Cafe/OS/RPL/COSModule.h"

bool mic_isActive(uint32 drcIndex);
void mic_updateOnAXFrame(); // called by snd_core once per AX frame

namespace mic
{
	COSModule* GetModule();
}
