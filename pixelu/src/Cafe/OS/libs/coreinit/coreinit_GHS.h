// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Support routines for the Green Hills C/C++ runtime that titles link against:
// stdio stream locks, the global runtime lock, errno, exception-handling globals
// and thread-local storage lookup.
namespace coreinit
{
	// Sets up the guest-visible runtime data (stdio streams, errno, environ).
	// Called once while coreinit is being mapped.
	void PrepareGHSRuntime();

	// errno of the calling guest thread
	sint32be* __gh_errno_ptr();
	void __gh_set_errno(sint32 errNo);
	sint32 __gh_get_errno();

	void InitializeGHS();
};
