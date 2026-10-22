// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Thread.h"

#include <string>

#ifdef _WIN32
#include <windows.h>

#include "Common/StringUtil.h"
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

#ifdef USE_VTUNE
#include <ittnotify.h>
#pragma comment(lib, "libittnotify.lib")
#endif

namespace Common
{
#ifdef _WIN32

int CurrentThreadId()
{
  return static_cast<int>(GetCurrentThreadId());
}

void SetThreadAffinity(std::thread::native_handle_type thread, u32 mask)
{
  SetThreadAffinityMask(reinterpret_cast<HANDLE>(thread), mask);
}

void SetCurrentThreadAffinity(u32 mask)
{
  SetThreadAffinityMask(GetCurrentThread(), mask);
}

void SleepCurrentThread(int ms)
{
  Sleep(static_cast<DWORD>(ms));
}

void SwitchCurrentThread()
{
  SwitchToThread();
}

namespace
{
#ifdef _MSC_VER
// Older debuggers learn thread names from a special exception (code 0x406D1388) that carries a
// THREADNAME_INFO record; see "How to: Set a thread name in native code" in the MSVC docs.
void NameThreadForOldDebuggers(const char* name)
{
#pragma pack(push, 8)
  struct
  {
    DWORD type;       // always 0x1000
    LPCSTR name;
    DWORD thread_id;  // -1 = calling thread
    DWORD flags;      // reserved, 0
  } record{0x1000, name, static_cast<DWORD>(-1), 0};
#pragma pack(pop)

  __try
  {
    RaiseException(0x406D1388, 0, sizeof(record) / sizeof(ULONG_PTR),
                   reinterpret_cast<const ULONG_PTR*>(&record));
  }
  __except (EXCEPTION_CONTINUE_EXECUTION)
  {
  }
}
#endif

// SetThreadDescription (Windows 10 1607+) keeps the name even for debuggers attached later. It
// is looked up at run time because some Windows Server builds lack the export.
void NameThreadViaDescription(const char* name)
{
  using SetDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  static const auto set_description = reinterpret_cast<SetDescriptionFn>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
  if (set_description)
    set_description(GetCurrentThread(), UTF8ToWString(name).c_str());
}
}  // namespace

void SetCurrentThreadName(const char* name)
{
#ifdef _MSC_VER
  NameThreadForOldDebuggers(name);
#endif
  NameThreadViaDescription(name);
#ifdef USE_VTUNE
  __itt_thread_set_name(name);
#endif
}

#else  // POSIX (Linux) fallback, used for development builds and tests only

int CurrentThreadId()
{
  return 0;
}

void SetThreadAffinity(std::thread::native_handle_type thread, u32 mask)
{
#if defined(__linux__) && !defined(__ANDROID__)
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  for (u32 cpu = 0; cpu < 32; ++cpu)
  {
    if (mask & (1u << cpu))
      CPU_SET(cpu, &cpus);
  }
  pthread_setaffinity_np(thread, sizeof(cpus), &cpus);
#else
  static_cast<void>(thread);
  static_cast<void>(mask);
#endif
}

void SetCurrentThreadAffinity(u32 mask)
{
  SetThreadAffinity(pthread_self(), mask);
}

void SleepCurrentThread(int ms)
{
  usleep(static_cast<useconds_t>(ms) * 1000);
}

void SwitchCurrentThread()
{
  usleep(1000);
}

void SetCurrentThreadName(const char* name)
{
  // Linux limits names to 15 characters plus the terminator.
  const std::string truncated = std::string(name).substr(0, 15);
  pthread_setname_np(pthread_self(), truncated.c_str());
#ifdef USE_VTUNE
  __itt_thread_set_name(name);
#endif
}

std::tuple<void*, size_t> GetCurrentThreadStack()
{
  void* low = nullptr;
  size_t size = 0;
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0)
  {
    pthread_attr_getstack(&attr, &low, &size);
    pthread_attr_destroy(&attr);
  }
  return {low, size};
}

#endif
}  // namespace Common
