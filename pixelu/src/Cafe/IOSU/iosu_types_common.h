// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

using IOSMsgQueueId = uint32;
using IOSTimerId = uint32;

static constexpr IOSTimerId IOSInvalidTimerId = 0xFFFFFFFF;

// Status codes of IOSU kernel calls. Negative means failure.
enum IOS_ERROR : sint32
{
	IOS_ERROR_OK = 0,
	IOS_ERROR_INVALID = -4,
	IOS_ERROR_MAXIMUM_REACHED = -5,
	IOS_ERROR_NONE_AVAILABLE = -7, // non-blocking receive on an empty queue
	IOS_ERROR_WOULD_BLOCK = -8,    // non-blocking send on a full queue
	IOS_ERROR_INVALID_ARG = -29,
};

inline bool IOS_ResultIsError(const IOS_ERROR err)
{
	return (err & 0x80000000) != 0;
}

// Lifecycle hooks for an emulated IOSU service. Every hook is optional.
class IOSUModule
{
  public:
	virtual void SystemLaunch() {}; // emulated system came up
	virtual void SystemExit() {};   // emulated system is shutting down
	virtual void TitleStart() {};   // a foreground title starts
	virtual void TitleStop() {};    // the foreground title ended
};
