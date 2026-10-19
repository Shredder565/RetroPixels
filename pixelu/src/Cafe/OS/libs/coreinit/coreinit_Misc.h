// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

namespace coreinit
{
	// process and title information
	uint32 OSGetUPID();
	uint32 OSGetPFID();
	uint64 OSGetTitleID();
	uint32 __OSGetProcessSDKVersion();

	// relaunching: both suspend the calling thread and return 0 on success
	uint32 OSLaunchTitleByPathl(const char* path, uint32 pathLength, uint32 argc);
	uint32 OSRestartGame(uint32 argc, MEMPTR<char>* argv);

	void OSReleaseForeground();

	// queues a release-foreground then acquire-foreground message pair on the system message queue
	void StartBackgroundForegroundTransition();

	// driver callbacks passed to OSDriver_Register
	struct OSDriverInterface
	{
		/* +0x00 */ MEMPTR<void> getDriverName;
		/* +0x04 */ MEMPTR<void> init;
		/* +0x08 */ MEMPTR<void> onAcquireForeground;
		/* +0x0C */ MEMPTR<void> onReleaseForeground;
		/* +0x10 */ MEMPTR<void> done;
	};
	static_assert(sizeof(OSDriverInterface) == 0x14);

	uint32 OSDriver_Register(uint32 moduleHandle, sint32 priority, OSDriverInterface* driverCallbacks, sint32 driverId, uint32be* outUkn1, uint32be* outUkn2, uint32be* outUkn3);
	uint32 OSDriver_Deregister(uint32 moduleHandle, sint32 driverId);

	enum class COSReportModule
	{
		coreinit = 0,
	};

	enum class COSReportLevel
	{
		Error = 0,
		Warn = 1,
		Info = 2
	};

	void OSFatal(const char* msg);

	// guest printf: formats into strOut (always terminated) and returns the number of characters stored
	sint32 ppc_vprintf(const char* formatStr, char* strOut, sint32 maxLength, ppc_va_list* vargs);

	void miscInit();
};
