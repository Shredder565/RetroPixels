// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Cooperative user-mode thread. A thread first wraps itself with
// PrepareCurrentThread(), then hops between fibers with Switch(). Each fiber
// carries a private pointer readable with GetFiberPrivateData() while it runs.
// The entry point must never return.
class Fiber
{
public:
	// new fiber with its own 2MB stack; on arm64 the entry point receives
	// userParam split into two uint32 arguments (high word, low word)
	Fiber(void(*FiberEntryPoint)(void* userParam), void* userParam, void* privateData);
	~Fiber();

	Fiber(const Fiber&) = delete;
	Fiber& operator=(const Fiber&) = delete;

	static Fiber* PrepareCurrentThread(void* privateData = nullptr);
	static void Switch(Fiber& targetFiber);
	static void* GetFiberPrivateData();

private:
	explicit Fiber(void* privateData); // adopts the calling thread

	void* m_implData{ nullptr }; // platform context / fiber handle
	void* m_privateData;
	void* m_stackPtr{ nullptr }; // owned stack, null for an adopted thread
};
