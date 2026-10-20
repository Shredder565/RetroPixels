// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/LdrWatcher.h"

#include <windows.h>
#include <cwchar>
#include <string>
#include <string_view>
#include <tlhelp32.h>
#include <winternl.h>

// The loader's DLL notification interface (ntdll, Vista and later). It is documented, but not
// declared in the SDK headers, so the layouts are spelled out here.
namespace
{
struct DllNotificationData
{
  ULONG flags;
  PCUNICODE_STRING full_dll_name;
  PCUNICODE_STRING base_dll_name;
  PVOID dll_base;
  ULONG size_of_image;
};

// Same layout for the "loaded" and "unloaded" reasons.
using DllNotificationCallbackFn = VOID(NTAPI*)(ULONG reason, const DllNotificationData* data,
                                               PVOID context);
using RegisterDllNotificationFn = NTSTATUS(NTAPI*)(ULONG flags, DllNotificationCallbackFn callback,
                                                   PVOID context, PVOID* cookie);
using UnregisterDllNotificationFn = NTSTATUS(NTAPI*)(PVOID cookie);

constexpr ULONG DLL_NOTIFICATION_REASON_LOADED = 1;

// Runs the observer's action for each of its names equal (ignoring case) to `module_name`.
void NotifyIfWatched(const LdrObserver& observer, std::wstring_view module_name,
                     uintptr_t base_address)
{
  for (const std::wstring& wanted : observer.module_names)
  {
    if (wanted.size() == module_name.size() &&
        _wcsnicmp(wanted.c_str(), module_name.data(), module_name.size()) == 0)
    {
      observer.action({wanted, base_address});
    }
  }
}

std::wstring_view View(PCUNICODE_STRING str)
{
  if (str == nullptr || str->Buffer == nullptr)
    return {};
  return {str->Buffer, str->Length / sizeof(wchar_t)};
}

VOID NTAPI OnDllNotification(ULONG reason, const DllNotificationData* data, PVOID context)
{
  if (reason != DLL_NOTIFICATION_REASON_LOADED)
    return;
  NotifyIfWatched(*static_cast<const LdrObserver*>(context), View(data->base_dll_name),
                  reinterpret_cast<uintptr_t>(data->dll_base));
}

// Process-wide access to the ntdll registration functions. Does nothing if they are missing.
class DllNotifications
{
public:
  static DllNotifications& Get()
  {
    static DllNotifications instance;
    return instance;
  }

  void Register(LdrObserver* observer)
  {
    PVOID cookie = nullptr;
    if (!m_register || !NT_SUCCESS(m_register(0, OnDllNotification, observer, &cookie)))
      cookie = nullptr;
    observer->cookie = cookie;
  }

  void Unregister(LdrObserver* observer)
  {
    if (m_unregister && observer->cookie)
      m_unregister(observer->cookie);
    observer->cookie = nullptr;
  }

private:
  DllNotifications()
  {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll");
    if (!ntdll)
      return;
    const auto reg = reinterpret_cast<RegisterDllNotificationFn>(
        GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    const auto unreg = reinterpret_cast<UnregisterDllNotificationFn>(
        GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
    if (reg && unreg)
    {
      m_register = reg;
      m_unregister = unreg;
    }
  }

  RegisterDllNotificationFn m_register = nullptr;
  UnregisterDllNotificationFn m_unregister = nullptr;
};
}  // namespace

LdrWatcher::~LdrWatcher()
{
  UninstallAll();
}

bool LdrWatcher::InjectCurrentModules(const LdrObserver& observer)
{
  // Toolhelp avoids a psapi dependency.
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE)
    return false;

  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry))
  {
    NotifyIfWatched(observer, std::wstring_view(entry.szModule),
                    reinterpret_cast<uintptr_t>(entry.modBaseAddr));
  }

  CloseHandle(snapshot);
  return true;
}

void LdrWatcher::Install(const LdrObserver& observer)
{
  LdrObserver& installed = observers.emplace_back(observer);
  // Register first, then scan: no module can slip through between the two (at worst one shows
  // up twice).
  DllNotifications::Get().Register(&installed);
  InjectCurrentModules(installed);
}

void LdrWatcher::UninstallAll()
{
  for (LdrObserver& observer : observers)
    DllNotifications::Get().Unregister(&observer);
  observers.clear();
}
