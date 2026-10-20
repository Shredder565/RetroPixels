// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/MemoryUtil.h"

#include <cstddef>
#include <cstdlib>
#include <string>

#include "Common/CommonFuncs.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/sysinfo.h>
#endif

namespace Common
{
namespace
{
std::string LastSystemError()
{
#ifdef _WIN32
  return GetLastErrorString();
#else
  return LastStrerrorString();
#endif
}

#ifdef _WIN32
bool Protect(void* ptr, size_t size, DWORD protection)
{
  DWORD previous;
  return VirtualProtect(ptr, size, protection, &previous) != FALSE;
}
#else
bool Protect(void* ptr, size_t size, int protection)
{
  return mprotect(ptr, size, protection) == 0;
}

void* MapAnonymous(size_t size, int protection)
{
  void* const ptr = mmap(nullptr, size, protection, MAP_ANON | MAP_PRIVATE, -1, 0);
  return ptr == MAP_FAILED ? nullptr : ptr;
}
#endif

// Nesting depth of JITPageWriteEnableExecuteDisable() on this thread. A function-local static so
// it is usable from other static initialisers.
int& WriteNesting()
{
  thread_local int depth = 0;
  return depth;
}
}  // namespace

void* AllocateExecutableMemory(size_t size)
{
#ifdef _WIN32
  void* const ptr = VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
  void* const ptr = MapAnonymous(size, PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
  if (!ptr)
    PanicAlertFmt("Failed to allocate executable memory");
  return ptr;
}

// The supported platforms let a thread write and execute JIT pages at the same time, so these
// only keep the nesting count (and catch unbalanced calls).
void JITPageWriteEnableExecuteDisable()
{
  ++WriteNesting();
}

void JITPageWriteDisableExecuteEnable()
{
  if (--WriteNesting() < 0)
    PanicAlertFmt("JITPageWriteDisableExecuteEnable() called more often than "
                  "JITPageWriteEnableExecuteDisable()");
}

void* AllocateMemoryPages(size_t size)
{
#ifdef _WIN32
  void* const ptr = VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_READWRITE);
#else
  void* const ptr = MapAnonymous(size, PROT_READ | PROT_WRITE);
#endif
  if (!ptr)
    PanicAlertFmt("Failed to allocate memory pages");
  return ptr;
}

bool FreeMemoryPages(void* ptr, size_t size)
{
  if (!ptr)
    return true;
#ifdef _WIN32
  const bool ok = VirtualFree(ptr, 0, MEM_RELEASE) != FALSE;
#else
  const bool ok = munmap(ptr, size) == 0;
#endif
  if (!ok)
    PanicAlertFmt("Freeing memory pages failed: {}", LastSystemError());
  return ok;
}

void* AllocateAlignedMemory(size_t size, size_t alignment)
{
#ifdef _WIN32
  void* const ptr = _aligned_malloc(size, alignment);
#else
  void* ptr = nullptr;
  if (posix_memalign(&ptr, alignment, size) != 0)
  {
    ERROR_LOG_FMT(MEMMAP, "posix_memalign({}, {}) failed", alignment, size);
    ptr = nullptr;
  }
#endif
  if (!ptr)
    PanicAlertFmt("Failed to allocate aligned memory");
  return ptr;
}

void FreeAlignedMemory(void* ptr)
{
  if (!ptr)
    return;
#ifdef _WIN32
  _aligned_free(ptr);
#else
  std::free(ptr);
#endif
}

bool ReadProtectMemory(void* ptr, size_t size)
{
#ifdef _WIN32
  const bool ok = Protect(ptr, size, PAGE_NOACCESS);
#else
  const bool ok = Protect(ptr, size, PROT_NONE);
#endif
  if (!ok)
    PanicAlertFmt("ReadProtectMemory failed: {}", LastSystemError());
  return ok;
}

bool WriteProtectMemory(void* ptr, size_t size, bool executable)
{
#ifdef _WIN32
  const bool ok = Protect(ptr, size, executable ? PAGE_EXECUTE_READ : PAGE_READONLY);
#else
  const bool ok = Protect(ptr, size, executable ? (PROT_READ | PROT_EXEC) : PROT_READ);
#endif
  if (!ok)
    PanicAlertFmt("WriteProtectMemory failed: {}", LastSystemError());
  return ok;
}

bool UnWriteProtectMemory(void* ptr, size_t size, bool allowExecute)
{
#ifdef _WIN32
  const bool ok = Protect(ptr, size, allowExecute ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
#else
  const bool ok = Protect(ptr, size,
                          allowExecute ? (PROT_READ | PROT_WRITE | PROT_EXEC) :
                                         (PROT_READ | PROT_WRITE));
#endif
  if (!ok)
    PanicAlertFmt("UnWriteProtectMemory failed: {}", LastSystemError());
  return ok;
}

size_t MemPhysical()
{
#ifdef _WIN32
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  GlobalMemoryStatusEx(&status);
  return static_cast<size_t>(status.ullTotalPhys);
#else
  struct sysinfo info{};
  sysinfo(&info);
  return static_cast<size_t>(info.totalram) * info.mem_unit;
#endif
}

}  // namespace Common
