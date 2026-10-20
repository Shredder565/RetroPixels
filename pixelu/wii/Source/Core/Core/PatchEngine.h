// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Common
{
class IniFile;
}
namespace Core
{
class System;
}

// Game patches: the [OnFrame] sections of game inis, plus the debugger's memory patches. They are
// applied once per frame, together with the AR and Gecko cheats.
namespace PatchEngine
{
// Written as "byte", "word" and "dword" in inis.
enum class PatchType
{
  Patch8Bit,
  Patch16Bit,
  Patch32Bit,
};

// One write. A conditional entry only writes while memory holds `comparand`.
struct PatchEntry
{
  PatchEntry() = default;
  PatchEntry(PatchType patch_type, u32 entry_address, u32 entry_value)
      : type(patch_type), address(entry_address), value(entry_value)
  {
  }
  PatchType type = PatchType::Patch8Bit;
  u32 address = 0;
  u32 value = 0;
  u32 comparand = 0;
  bool conditional = false;
};

struct Patch
{
  std::string name;
  std::vector<PatchEntry> entries;
  bool enabled = false;
  bool default_enabled = false;
  // False for patches from the global (shipped) game ini.
  bool user_defined = false;
};

const char* PatchTypeAsString(PatchType type);

// Entry lines are "address:type:value" or "address:type:value:comparand"; the first ':' may be
// written as '='.
std::optional<PatchEntry> DeserializeLine(std::string line);
std::string SerializeLine(const PatchEntry& entry);
void LoadPatchSection(const std::string& section, std::vector<Patch>* patches,
                      const Common::IniFile& globalIni, const Common::IniFile& localIni);
void SavePatchSection(Common::IniFile* local_ini, std::span<const Patch> patches);
// Loads this game's patches and cheats.
void LoadPatches();

// Debugger memory patches (by index into the debug interface's patch list) to apply each frame.
void AddMemoryPatch(std::size_t index);
void RemoveMemoryPatch(std::size_t index);

// Returns false when the CPU is in a state where nothing can be applied yet (try again later).
bool ApplyFramePatches(Core::System& system);
void Shutdown();
void Reload(Core::System& system);

// Hex digits of a value of the given type.
inline int GetPatchTypeCharLength(PatchType type)
{
  switch (type)
  {
  case PatchType::Patch8Bit:
    return 2;
  case PatchType::Patch16Bit:
    return 4;
  case PatchType::Patch32Bit:
    return 8;
  }
  return 8;
}

}  // namespace PatchEngine
