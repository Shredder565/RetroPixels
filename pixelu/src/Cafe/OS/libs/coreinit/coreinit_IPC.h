// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/IOSU/iosu_ipc_common.h"
#include "Cafe/IOSU/iosu_types_common.h"

namespace coreinit
{
	// entry point for the IOSU side: delivers finished requests back to the PPC core that sent them
	void IPCDriver_NotifyResponses(uint32 ppcCoreIndex, IPCCommandBody** responseArray, uint32 numResponses);

	// IOS resource manager requests. The synchronous variants block until IOSU replies; the async
	// variants return right away and the result is later passed to asyncResultFunc(result, userParam)
	// on the core's IPC thread.
	IOSDevHandle IOS_Open(const char* devicePath, uint32 flags);
	IOS_ERROR IOS_Close(IOSDevHandle devHandle);
	IOS_ERROR IOS_Ioctl(IOSDevHandle devHandle, uint32 requestId, void* ptrIn, uint32 sizeIn, void* ptrOut, uint32 sizeOut);
	IOS_ERROR IOS_IoctlAsync(IOSDevHandle devHandle, uint32 requestId, void* ptrIn, uint32 sizeIn, void* ptrOut, uint32 sizeOut, MEMPTR<void> asyncResultFunc, MEMPTR<void> asyncResultUserParam);
	IOS_ERROR IOS_Ioctlv(IOSDevHandle devHandle, uint32 requestId, uint32 numIn, uint32 numOut, IPCIoctlVector* vec);
	IOS_ERROR IOS_IoctlvAsync(IOSDevHandle devHandle, uint32 requestId, uint32 numIn, uint32 numOut, IPCIoctlVector* vec, MEMPTR<void> asyncResultFunc, MEMPTR<void> asyncResultUserParam);

	void MapIPCExports();
	void InitializeIPC();
};
