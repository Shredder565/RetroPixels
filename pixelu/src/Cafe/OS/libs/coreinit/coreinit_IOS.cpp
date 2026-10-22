// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/libs/coreinit/coreinit_IOS.h"
#include "Cafe/IOSU/legacy/iosu_ioctl.h"

// Legacy device request path used by HLE libraries that talk to the old IOSU device threads.
// Guest code goes through coreinit_IPC instead.

namespace
{
	struct LegacyDevice
	{
		const char* path;
		sint32 handle;
	};

	constexpr LegacyDevice s_legacyDevices[] =
	{
		{ IOS_PATH_ODM, IOS_DEVICE_ODM },
		{ IOS_PATH_SOCKET, IOS_DEVICE_SOCKET },
		{ IOS_PATH_ACT, IOS_DEVICE_ACT },
		{ IOS_PATH_FPD, IOS_DEVICE_FPD },
		{ IOS_PATH_ACP_MAIN, IOS_DEVICE_ACP_MAIN },
		{ IOS_PATH_MCP, IOS_DEVICE_MCP },
		{ IOS_PATH_BOSS, IOS_DEVICE_BOSS },
		{ IOS_PATH_NIM, IOS_DEVICE_NIM },
		{ IOS_PATH_IOSUHAX, -1 }, // never available
	};

	constexpr uint32 kOdmGetDriveState = 4;
	constexpr uint32 kOdmDriveStateReady = 0xA;
}

sint32 __depr__IOS_Open(char* path, uint32 mode)
{
	if (!path)
		return 0;
	for (const LegacyDevice& dev : s_legacyDevices)
	{
		if (strcmp(path, dev.path) == 0)
			return dev.handle;
	}
	return IOS_DEVICE_UKN;
}

sint32 __depr__IOS_Ioctl(uint32 fd, uint32 request, void* inBuffer, uint32 inSize, void* outBuffer, uint32 outSize)
{
	if (fd == IOS_DEVICE_ODM)
	{
		// the system menu polls the disc drive state
		if (request == kOdmGetDriveState)
			*(uint32be*)outBuffer = kOdmDriveStateReady;
		return 0;
	}
	cemuLog_logDebug(LogType::Force, "__depr__IOS_Ioctl: device {} request {} not handled", fd, request);
	return 0;
}

sint32 __depr__IOS_Ioctlv(uint32 fd, uint32 request, uint32 countIn, uint32 countOut, ioBufferVector_t* ioBufferVectors)
{
	// the request lives on the guest stack while the IOSU device thread processes it
	StackAllocator<ioQueueEntry_t> entry;
	ioQueueEntry_t* e = entry.GetPointer();
	e->isIoctlv = true;
	e->isAsync = false;
	e->request = request;
	e->countIn = countIn;
	e->countOut = countOut;
	e->bufferVectors = ioBufferVectors;
	e->ppcThread = nullptr;
	e->isCompleted = false;
	e->returnValue = 0;
	return iosuIoctl_pushAndWait(fd, e);
}

sint32 __depr__IOS_Close(uint32 fd)
{
	return 0;
}
