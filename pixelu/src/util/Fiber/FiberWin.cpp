// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Fiber.h"
#include <Windows.h>

// fiber currently running on this thread
thread_local Fiber* sCurrentFiber{};

Fiber::Fiber(void(*FiberEntryPoint)(void* userParam), void* userParam, void* privateData) : m_privateData(privateData)
{
	m_implData = CreateFiber(2 * 1024 * 1024, reinterpret_cast<LPFIBER_START_ROUTINE>(FiberEntryPoint), userParam);
}

Fiber::Fiber(void* privateData) : m_privateData(privateData)
{
	m_implData = ConvertThreadToFiber(nullptr);
}

Fiber::~Fiber()
{
	if (m_implData == nullptr)
		return;
	if (sCurrentFiber == this)
	{
		// deleting the running fiber would end the thread; turn it back into a plain thread instead
		ConvertFiberToThread();
		sCurrentFiber = nullptr;
		return;
	}
	DeleteFiber(m_implData);
}

Fiber* Fiber::PrepareCurrentThread(void* privateData)
{
	cemu_assert_debug(sCurrentFiber == nullptr);
	sCurrentFiber = new Fiber(privateData);
	return sCurrentFiber;
}

void Fiber::Switch(Fiber& targetFiber)
{
	sCurrentFiber = &targetFiber;
	SwitchToFiber(targetFiber.m_implData);
}

void* Fiber::GetFiberPrivateData()
{
	return sCurrentFiber->m_privateData;
}
