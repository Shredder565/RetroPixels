// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Fiber.h"
#include <ucontext.h>
#include <atomic>

// fiber currently running on this thread
thread_local Fiber* sCurrentFiber{};

namespace
{
	constexpr size_t kFiberStackSize = 2 * 1024 * 1024;

	ucontext_t* asContext(void* p)
	{
		return static_cast<ucontext_t*>(p);
	}
}

Fiber::Fiber(void(*FiberEntryPoint)(void* userParam), void* userParam, void* privateData) : m_privateData(privateData)
{
	auto* ctx = static_cast<ucontext_t*>(calloc(1, sizeof(ucontext_t)));
	m_stackPtr = malloc(kFiberStackSize);
	getcontext(ctx);
	ctx->uc_stack.ss_sp = m_stackPtr;
	ctx->uc_stack.ss_size = kFiberStackSize;
	ctx->uc_link = nullptr;
	auto entry = reinterpret_cast<void(*)()>(FiberEntryPoint);
#ifdef __arm64__
	// makecontext only passes int-sized arguments portably, so split the pointer
	const uint64 bits = (uint64)(uintptr_t)userParam;
	makecontext(ctx, entry, 2, (uint32)(bits >> 32), (uint32)bits);
#else
	makecontext(ctx, entry, 1, userParam);
#endif
	m_implData = ctx;
}

Fiber::Fiber(void* privateData) : m_privateData(privateData)
{
	auto* ctx = static_cast<ucontext_t*>(calloc(1, sizeof(ucontext_t)));
	getcontext(ctx);
	m_implData = ctx;
}

Fiber::~Fiber()
{
	free(m_stackPtr);
	free(m_implData);
}

Fiber* Fiber::PrepareCurrentThread(void* privateData)
{
	cemu_assert_debug(sCurrentFiber == nullptr);
	sCurrentFiber = new Fiber(privateData);
	return sCurrentFiber;
}

void Fiber::Switch(Fiber& targetFiber)
{
	Fiber* from = sCurrentFiber;
	sCurrentFiber = &targetFiber;
	// keep the compiler/CPU from moving thread_local accesses across the switch
	std::atomic_thread_fence(std::memory_order_seq_cst);
	swapcontext(asContext(from->m_implData), asContext(targetFiber.m_implData));
	std::atomic_thread_fence(std::memory_order_seq_cst);
}

void* Fiber::GetFiberPrivateData()
{
	return sCurrentFiber->m_privateData;
}
