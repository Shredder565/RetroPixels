// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#ifndef NOMINMAX
	#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

// names of the BSD-style Bluetooth constants
#define AF_BLUETOOTH AF_BTH
#define BTPROTO_RFCOMM BT_PORT_ANY

// reader/writer lock on top of the Windows SRW lock
class SlimRWLock
{
public:
	SlimRWLock();

	void LockRead();
	void UnlockRead();
	void LockWrite();
	void UnlockWrite();

private:
	SRWLOCK m_lock;
};

// GetLastError() of the calling thread
uint32_t GetExceptionError();
