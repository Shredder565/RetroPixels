// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "coreinit_BSP.h"

// Board support package queries
namespace coreinit
{
	// The top byte is the hardware generation; values below 0x11 identify a Hollywood (Wii) board,
	// so report the first Latte generation
	bool bspGetHardwareVersion(uint32be* version)
	{
		constexpr uint32 kLatteGeneration = 0x11;
		*version = kLatteGeneration << 24;
		return true;
	}

	void InitializeBSP()
	{
		cafeExportRegister("coreinit", bspGetHardwareVersion, LogType::Placeholder);
	}
}
