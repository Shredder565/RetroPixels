// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Debug/MemoryPatches.h"

#include <utility>

namespace Common::Debug
{
MemoryPatch::MemoryPatch(u32 address_, std::vector<u8> value_)
    : address(address_), value(std::move(value_))
{
}

MemoryPatch::MemoryPatch(u32 address_, u32 value_)
    : address(address_), value{static_cast<u8>(value_ >> 24), static_cast<u8>(value_ >> 16),
                               static_cast<u8>(value_ >> 8), static_cast<u8>(value_)}
{
}

MemoryPatches::MemoryPatches() = default;
MemoryPatches::~MemoryPatches() = default;

void MemoryPatches::Append(const Core::CPUThreadGuard& guard, MemoryPatch patch,
                           MemoryPatch::ApplyType type)
{
  patch.type = type;
  m_patches.push_back(std::move(patch));
  Patch(guard, m_patches.size() - 1);
}

// The 32-bit forms add a patch even when one already exists at the address (the debugger's
// "patch instruction" commands stack); the byte-vector forms replace an existing one.
void MemoryPatches::SetPatch(const Core::CPUThreadGuard& guard, u32 address, u32 value)
{
  Append(guard, MemoryPatch(address, value), MemoryPatch::ApplyType::Once);
}

void MemoryPatches::SetPatch(const Core::CPUThreadGuard& guard, u32 address, std::vector<u8> value)
{
  UnsetPatch(guard, address);
  Append(guard, MemoryPatch(address, std::move(value)), MemoryPatch::ApplyType::Once);
}

void MemoryPatches::SetFramePatch(const Core::CPUThreadGuard& guard, u32 address, u32 value)
{
  Append(guard, MemoryPatch(address, value), MemoryPatch::ApplyType::EachFrame);
}

void MemoryPatches::SetFramePatch(const Core::CPUThreadGuard& guard, u32 address,
                                  std::vector<u8> value)
{
  UnsetPatch(guard, address);
  Append(guard, MemoryPatch(address, std::move(value)), MemoryPatch::ApplyType::EachFrame);
}

const std::vector<MemoryPatch>& MemoryPatches::GetPatches() const
{
  return m_patches;
}

void MemoryPatches::UnsetPatch(const Core::CPUThreadGuard& guard, u32 address)
{
  // Only the first patch at this address goes.
  for (std::size_t i = 0; i < m_patches.size(); ++i)
  {
    if (m_patches[i].address == address)
    {
      RemovePatch(guard, i);
      return;
    }
  }
}

void MemoryPatches::EnablePatch(const Core::CPUThreadGuard& guard, std::size_t index)
{
  MemoryPatch& patch = m_patches[index];
  if (patch.is_enabled != MemoryPatch::State::Enabled)
  {
    patch.is_enabled = MemoryPatch::State::Enabled;
    Patch(guard, index);
  }
}

void MemoryPatches::DisablePatch(const Core::CPUThreadGuard& guard, std::size_t index)
{
  MemoryPatch& patch = m_patches[index];
  if (patch.is_enabled != MemoryPatch::State::Disabled)
  {
    patch.is_enabled = MemoryPatch::State::Disabled;
    Patch(guard, index);
  }
}

bool MemoryPatches::HasEnabledPatch(u32 address) const
{
  for (const MemoryPatch& patch : m_patches)
  {
    if (patch.address == address && patch.is_enabled == MemoryPatch::State::Enabled)
      return true;
  }
  return false;
}

void MemoryPatches::RemovePatch(const Core::CPUThreadGuard& guard, std::size_t index)
{
  DisablePatch(guard, index);
  UnPatch(index);
  m_patches.erase(m_patches.begin() + static_cast<std::ptrdiff_t>(index));
}

void MemoryPatches::ClearPatches(const Core::CPUThreadGuard& guard)
{
  for (std::size_t i = 0; i < m_patches.size(); ++i)
  {
    DisablePatch(guard, i);
    UnPatch(i);
  }
  m_patches.clear();
}
}  // namespace Common::Debug
