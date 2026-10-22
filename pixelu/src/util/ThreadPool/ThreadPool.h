// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include <thread>

class ThreadPool
{
public:
	// runs f(args...) on a new detached thread; arguments are moved or copied into it
	template<class TFunction, class... TArgs>
	static void FireAndForget(TFunction&& f, TArgs&&... args)
	{
		std::thread(std::forward<TFunction>(f), std::forward<TArgs>(args)...).detach();
	}
};
