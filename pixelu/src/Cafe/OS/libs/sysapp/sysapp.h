// Original code for RetroPixels (PixelU). Not derived from Cemu.
// sysapp: system applications (title ids, launch arguments).
#pragma once
#include "Cafe/OS/RPL/COSModule.h"

namespace sysapp
{
	COSModule* GetModule();
}

// Title id of system application <index> (0 = Wii U Menu) for the console's region.
uint64 _SYSGetSystemApplicationTitleId(sint32 index);
