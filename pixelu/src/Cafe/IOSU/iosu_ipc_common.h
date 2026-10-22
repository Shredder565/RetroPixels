// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

using IOSDevHandle = uint32;

enum class IPCDriverState : uint32
{
	CLOSED = 1,
	INITIALIZED = 2,
	READY = 3,
	SUBMITTING = 4
};

enum class IPCCommandId : uint32
{
	IOS_OPEN = 1,
	IOS_CLOSE = 2,
	IOS_IOCTL = 6,
	IOS_IOCTLV = 7,
};

// One buffer of an ioctlv request
struct IPCIoctlVector
{
	MEMPTR<void> baseVirt;
	uint32be size;
	MEMPTR<void> basePhys;
};
static_assert(sizeof(IPCIoctlVector) == 0xC);

// Request block shared between the PPC IPC driver and IOSU (guest memory, big endian).
//
// Argument use per command:
//   IOS_OPEN   args[0] path, args[1] path length including terminator, args[2] mode
//   IOS_CLOSE  no arguments, devHandle only
//   IOS_IOCTL  args[0] request, args[1] input ptr, args[2] input size, args[3] output ptr, args[4] output size
//   IOS_IOCTLV args[0] request, args[1] input vector count, args[2] output vector count, args[3] IPCIoctlVector array
struct IPCCommandBody
{
	betype<IPCCommandId> cmdId;
	uint32be result; // written by IOSU when the request completes
	betype<IOSDevHandle> devHandle;
	uint32be ukn0C;
	uint32be ukn10;
	uint32be ukn14;
	uint32be ukn18;
	uint32be ukn1C;
	uint32be ukn20;
	uint32be args[5];
	// fields after this point are bookkeeping for the PPC side driver
	betype<IPCCommandId> prev_cmdId;
	betype<IOSDevHandle> prev_devHandle;
	MEMPTR<void> ppcVirt0;
	MEMPTR<void> ppcVirt1;
};
static_assert(offsetof(IPCCommandBody, args) == 0x24);
static_assert(offsetof(IPCCommandBody, prev_cmdId) == 0x38);
static_assert(sizeof(IPCCommandBody) == 0x48);
