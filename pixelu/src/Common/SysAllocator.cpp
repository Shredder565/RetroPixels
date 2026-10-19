// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "SysAllocator.h"

SysAllocatorContainer& SysAllocatorContainer::GetInstance()
{
	// function-local so that it exists before the first static SysAllocator registers
	static SysAllocatorContainer container;
	return container;
}

void SysAllocatorContainer::PushSysAllocator(SysAllocatorBase* base)
{
	m_entries.emplace_back(base);
}

void SysAllocatorContainer::Initialize()
{
	// registration order decides the guest addresses, keep it
	for (size_t i = 0; i < m_entries.size(); i++)
		m_entries[i]->Initialize();
}

SysAllocatorBase::SysAllocatorBase()
{
	SysAllocatorContainer::GetInstance().PushSysAllocator(this);
}
