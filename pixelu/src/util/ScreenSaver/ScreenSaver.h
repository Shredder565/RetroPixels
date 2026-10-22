// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cemu/Logging/CemuLogging.h"

#ifdef HAS_SDL
#include <SDL3/SDL.h>
#endif

// Turns the OS screen saver off (inhibit = true) or back on. SDL requires
// this to happen on the main thread, so the work is queued there and this
// call returns immediately.
class ScreenSaver
{
public:
#ifdef HAS_SDL
	static void SetInhibit(bool inhibit)
	{
		// the flag travels in the pointer value itself, no allocation needed
		void* arg = inhibit ? reinterpret_cast<void*>(uintptr_t{ 1 }) : nullptr;
		if (!SDL_RunOnMainThread(&ScreenSaver::ApplyOnMainThread, arg, false))
			cemuLog_log(LogType::Force, "ScreenSaver: could not queue the change on the main thread: {}", SDL_GetError());
	}

private:
	static void SDLCALL ApplyOnMainThread(void* arg)
	{
		const bool inhibit = arg != nullptr;
		if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && !SDL_InitSubSystem(SDL_INIT_VIDEO))
		{
			cemuLog_log(LogType::Force, "ScreenSaver: SDL video could not be initialized: {}", SDL_GetError());
			return;
		}
		const bool ok = inhibit ? SDL_DisableScreenSaver() : SDL_EnableScreenSaver();
		if (!ok)
		{
			cemuLog_log(LogType::Force, "ScreenSaver: could not {} the screen saver: {}", inhibit ? "disable" : "enable", SDL_GetError());
			return;
		}
		if (SDL_ScreenSaverEnabled() == inhibit)
			cemuLog_log(LogType::Force, "ScreenSaver: the screen saver is still {} after the change", inhibit ? "enabled" : "disabled");
	}
#else
	static void SetInhibit(bool /*inhibit*/) {}
#endif
};
