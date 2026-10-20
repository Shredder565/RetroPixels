// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Common::Debug
{
// One debugger memory patch: bytes to write at a guest address.
struct MemoryPatch
{
  enum class State
  {
    Enabled,
    Disabled
  };

  enum class ApplyType
  {
    Once,       // written when set or enabled
    EachFrame,  // written again every frame
  };

  MemoryPatch(u32 address_, std::vector<u8> value_);
  // A big-endian 32-bit value.
  MemoryPatch(u32 address_, u32 value_);

  u32 address;
  std::vector<u8> value;
  State is_enabled = State::Enabled;
  ApplyType type = ApplyType::Once;
};

// A list of patches. The derived class does the actual memory writes (Patch) and undoes them
// (UnPatch); this class keeps the list and the enabled/disabled bookkeeping.
class MemoryPatches
{
public:
  MemoryPatches();
  virtual ~MemoryPatches();

  void SetPatch(const Core::CPUThreadGuard& guard, u32 address, u32 value);
  void SetPatch(const Core::CPUThreadGuard& guard, u32 address, std::vector<u8> value);
  void SetFramePatch(const Core::CPUThreadGuard& guard, u32 address, u32 value);
  void SetFramePatch(const Core::CPUThreadGuard& guard, u32 address, std::vector<u8> value);
  const std::vector<MemoryPatch>& GetPatches() const;
  void UnsetPatch(const Core::CPUThreadGuard& guard, u32 address);
  void EnablePatch(const Core::CPUThreadGuard& guard, std::size_t index);
  void DisablePatch(const Core::CPUThreadGuard& guard, std::size_t index);
  bool HasEnabledPatch(u32 address) const;
  void RemovePatch(const Core::CPUThreadGuard& guard, std::size_t index);
  void ClearPatches(const Core::CPUThreadGuard& guard);
  virtual void ApplyExistingPatch(const Core::CPUThreadGuard& guard, std::size_t index) = 0;

protected:
  virtual void Patch(const Core::CPUThreadGuard& guard, std::size_t index) = 0;
  virtual void UnPatch(std::size_t index) = 0;

  std::vector<MemoryPatch> m_patches;

private:
  void Append(const Core::CPUThreadGuard& guard, MemoryPatch patch, MemoryPatch::ApplyType type);
};
}  // namespace Common::Debug
