// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "coreinit_OverlayArena.h"

// The overlay arena is an extra memory region a title can ask for once; enabling it maps the
// region and it stays mapped for the rest of the session
namespace coreinit
{
	namespace
	{
		bool s_overlayArenaEnabled = false;
	}

	uint32 OSIsEnabledOverlayArena()
	{
		return s_overlayArenaEnabled ? 1 : 0;
	}

	void OSEnableOverlayArena(uint32 uknParam, uint32be* areaOffset, uint32be* areaSize)
	{
		if (!s_overlayArenaEnabled)
		{
			memory_enableOverlayArena();
			s_overlayArenaEnabled = true;
		}
		*areaOffset = MEMORY_OVERLAY_AREA_OFFSET;
		*areaSize = MEMORY_OVERLAY_AREA_SIZE;
	}

	void InitializeOverlayArena()
	{
		cafeExportRegister("coreinit", OSIsEnabledOverlayArena, LogType::Placeholder);
		cafeExportRegister("coreinit", OSEnableOverlayArena, LogType::Placeholder);
		s_overlayArenaEnabled = false;
	}
}
