// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <string>
#include <vector>

// Calls back when a DLL with one of a set of names is (or already was) loaded into the process.

struct LdrDllLoadEvent
{
  const std::wstring& name;  // the matching entry of LdrObserver::module_names
  uintptr_t base_address;    // where the module is loaded
};

class LdrObserver
{
public:
  // Base file names to watch for, compared case-insensitively.
  std::vector<std::wstring> module_names;
  // May run inside the loader's notification callout (loader lock held), so it should do very
  // little and defer real work.
  std::function<void(const LdrDllLoadEvent&)> action;
  // Registration cookie; managed by LdrWatcher.
  void* cookie{};
};

class LdrWatcher
{
public:
  // Starts watching and immediately reports matching modules that are already loaded. A module
  // loading concurrently with this call may be reported twice.
  void Install(const LdrObserver& observer);
  ~LdrWatcher();

private:
  bool InjectCurrentModules(const LdrObserver& observer);
  void UninstallAll();

  // A list, so the addresses handed to the loader as callback context stay valid.
  std::list<LdrObserver> observers;
};
