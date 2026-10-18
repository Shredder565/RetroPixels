// Original code for RetroPixels (PixelU). Not derived from Cemu.
// zlib125: the console's zlib, backed by the host's zlib.
#pragma once
#include "Cafe/OS/RPL/COSModule.h"

namespace zlib
{
	void load();

	COSModule* GetModule();
}
