// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

namespace GX2
{
	void GX2Init_event(); // starts the guest thread that runs event callbacks
	void GX2EventResetToDefaultState();

	void GX2EventInit();
	void GX2WaitForVsync();
	void GX2WaitForFlip();
	bool GX2DrawDone();

	enum class GX2CallbackEventType
	{
		TIMESTAMP_TOP = 0,
		TIMESTAMP_BOTTOM = 1,
		VSYNC = 2,
		FLIP = 3,
		// 4: presumably command buffer overrun
	};
	inline constexpr size_t GX2CallbackEventTypeCount = 5;

	// called by the GPU emulation
	void __GX2NotifyNewRetirementTimestamp(uint64 tsRetire);
	void __GX2NotifyEvent(GX2CallbackEventType eventType);
}
