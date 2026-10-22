// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Common/precompiled.h"

// FileStream: the platform's file class (see FileStream_win32.h / FileStream_unix.h)
#if defined(_WIN32)
	#include "Common/windows/FileStream_win32.h"
#else
	#include "Common/unix/FileStream_unix.h"
#endif
