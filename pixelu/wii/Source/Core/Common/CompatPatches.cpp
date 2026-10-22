// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Run-time fixes for third-party DLLs that misbehave when loaded into our process.
//
// EZFRD64.dll / 811EZFRD64.DLL ship with many cheap third-party gamepads and adapters and get
// injected into every process that uses DirectInput. They truncate heap pointers to 32 bits, so
// they crash whenever their heap ends up above 4 GiB, which is the norm on 64-bit Windows. We
// redirect their HeapCreate import to a version that places the heap below 4 GiB. The patch only
// prevents the crash if it lands before the DLL initialises, i.e. when it is applied from the
// loader notification.

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/LdrWatcher.h"

namespace
{
// ntdll's RtlCreateHeap, which (unlike HeapCreate) can build a heap in memory we reserved.
using RtlCreateHeapFn = PVOID(NTAPI*)(ULONG flags, PVOID base, SIZE_T reserve_size,
                                      SIZE_T commit_size, PVOID lock, PVOID parameters);

HANDLE WINAPI HeapCreateBelow4GiB(DWORD /*options*/, SIZE_T /*initial_size*/,
                                  SIZE_T /*maximum_size*/)
{
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll)
    return nullptr;
  const auto create_heap = reinterpret_cast<RtlCreateHeapFn>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlCreateHeap")));
  if (!create_heap)
    return nullptr;

  // Reserve a 16 MiB region somewhere in the low 4 GiB, trying each allocation-granularity slot
  // from 2 MiB upwards.
  constexpr std::uintptr_t REGION_SIZE = 0x01000000;
  constexpr std::uintptr_t FIRST_ADDRESS = 0x00200000;
  constexpr std::uintptr_t LAST_ADDRESS = 0x100000000ull - REGION_SIZE;
  constexpr std::uintptr_t STEP = 0x10000;

  void* region = nullptr;
  for (std::uintptr_t address = FIRST_ADDRESS; !region && address <= LAST_ADDRESS;
       address += STEP)
  {
    region = VirtualAlloc(reinterpret_cast<void*>(address), REGION_SIZE, MEM_RESERVE,
                          PAGE_READWRITE);
  }
  if (!region)
    return nullptr;

  return create_heap(0, region, 0, 0, nullptr, nullptr);
}

// Replaces one entry of a loaded module's import address table. The module has already been
// parsed by the loader, so its headers are trusted.
bool ReplaceImport(std::uintptr_t module_base, const char* dll_name, const char* function_name,
                   void* replacement)
{
  const auto at = [module_base](DWORD rva) { return module_base + rva; };

  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(at(dos->e_lfanew));
  const IMAGE_DATA_DIRECTORY& imports =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

  for (auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
           at(imports.VirtualAddress));
       descriptor->OriginalFirstThunk != 0; ++descriptor)
  {
    if (_stricmp(reinterpret_cast<const char*>(at(descriptor->Name)), dll_name) != 0)
      continue;

    // The lookup table names the imports; the address table at the same index holds the
    // resolved pointer.
    const auto* lookup = reinterpret_cast<const IMAGE_THUNK_DATA*>(
        at(descriptor->OriginalFirstThunk));
    auto* address_table = reinterpret_cast<IMAGE_THUNK_DATA*>(at(descriptor->FirstThunk));

    for (std::size_t i = 0; lookup[i].u1.AddressOfData != 0; ++i)
    {
      if (IMAGE_SNAP_BY_ORDINAL(lookup[i].u1.Ordinal))
        continue;
      const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
          at(static_cast<DWORD>(lookup[i].u1.AddressOfData)));
      if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function_name) != 0)
        continue;

      // The IAT is usually read-only after binding.
      auto* slot = &address_table[i].u1.Function;
      DWORD previous_protection;
      if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous_protection))
        return false;
      *slot = reinterpret_cast<std::uintptr_t>(replacement);
      return VirtualProtect(slot, sizeof(*slot), previous_protection, &previous_protection) != 0;
    }
    return false;  // the DLL is imported, but not that function
  }
  return false;  // the DLL is not imported
}

void InstallCompatPatches(LdrWatcher& watcher)
{
  watcher.Install({{L"EZFRD64.dll", L"811EZFRD64.DLL"}, [](const LdrDllLoadEvent& event) {
                     ReplaceImport(event.base_address, "kernel32.dll", "HeapCreate",
                                   reinterpret_cast<void*>(&HeapCreateBelow4GiB));
                   }});
}
}  // namespace

int __cdecl EnableCompatPatches()
{
  static LdrWatcher watcher;
  InstallCompatPatches(watcher);
  return 0;
}

#ifdef _MSC_VER
// Run EnableCompatPatches during CRT start-up, as the last C++ initialiser (".CRT$XCZ"), so the
// watcher is in place before any DirectInput user can pull the DLLs in. The linker keeps the
// pointer because CMake passes /INCLUDE:enableCompatPatches.
#pragma section(".CRT$XCZ", read)
extern "C" {
__declspec(allocate(".CRT$XCZ")) decltype(&EnableCompatPatches) enableCompatPatches =
    EnableCompatPatches;
}
#else
extern "C" {
decltype(&EnableCompatPatches) enableCompatPatches = EnableCompatPatches;
}
static const int s_compat_patches_installed = EnableCompatPatches();
#endif
