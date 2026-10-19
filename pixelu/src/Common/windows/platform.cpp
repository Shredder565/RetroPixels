// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include <Windows.h>

SlimRWLock::SlimRWLock()
{
	InitializeSRWLock(&m_lock);
}

void SlimRWLock::LockRead()
{
	AcquireSRWLockShared(&m_lock);
}

void SlimRWLock::UnlockRead()
{
	ReleaseSRWLockShared(&m_lock);
}

void SlimRWLock::LockWrite()
{
	AcquireSRWLockExclusive(&m_lock);
}

void SlimRWLock::UnlockWrite()
{
	ReleaseSRWLockExclusive(&m_lock);
}

uint32_t GetExceptionError()
{
	return (uint32_t)GetLastError();
}
