// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Debug/Watches.h"

#include <algorithm>
#include <locale>
#include <sstream>
#include <utility>

#include <fmt/format.h>

namespace Common::Debug
{
Watch::Watch(u32 address_, std::string name_, State is_enabled_)
    : address(address_), name(std::move(name_)), is_enabled(is_enabled_)
{
}

std::size_t Watches::SetWatch(u32 address, std::string name)
{
  const auto existing = std::ranges::find(m_watches, address, &Watch::address);
  if (existing != m_watches.end())
  {
    const auto index = static_cast<std::size_t>(existing - m_watches.begin());
    UpdateWatch(index, address, std::move(name));
    return index;
  }

  m_watches.emplace_back(address, std::move(name), Watch::State::Enabled);
  return m_watches.size() - 1;
}

const Watch& Watches::GetWatch(std::size_t index) const
{
  return m_watches.at(index);
}

const std::vector<Watch>& Watches::GetWatches() const
{
  return m_watches;
}

void Watches::UnsetWatch(u32 address)
{
  std::erase_if(m_watches, [address](const Watch& w) { return w.address == address; });
}

void Watches::UpdateWatch(std::size_t index, u32 address, std::string name)
{
  Watch& watch = m_watches[index];
  watch.address = address;
  watch.name = std::move(name);
}

void Watches::UpdateWatchAddress(std::size_t index, u32 address)
{
  m_watches[index].address = address;
}

void Watches::UpdateWatchName(std::size_t index, std::string name)
{
  m_watches[index].name = std::move(name);
}

void Watches::UpdateWatchLockedState(std::size_t index, bool locked)
{
  m_watches[index].locked = locked;
}

void Watches::EnableWatch(std::size_t index)
{
  m_watches[index].is_enabled = Watch::State::Enabled;
}

void Watches::DisableWatch(std::size_t index)
{
  m_watches[index].is_enabled = Watch::State::Disabled;
}

bool Watches::HasEnabledWatch(u32 address) const
{
  return std::ranges::any_of(m_watches, [address](const Watch& w) {
    return w.address == address && w.is_enabled == Watch::State::Enabled;
  });
}

void Watches::RemoveWatch(std::size_t index)
{
  m_watches.erase(m_watches.begin() + static_cast<std::ptrdiff_t>(index));
}

void Watches::LoadFromStrings(std::span<const std::string> watches)
{
  for (const std::string& line : watches)
  {
    // Locale-independent "<hex address> <name to end of line>". A line that does not start
    // with a number becomes a watch at address 0 with an empty name.
    std::istringstream stream(line);
    stream.imbue(std::locale::classic());
    u32 address = 0;
    std::string name;
    stream >> std::hex >> address >> std::ws;
    std::getline(stream, name);
    SetWatch(address, std::move(name));
  }
}

std::vector<std::string> Watches::SaveToStrings() const
{
  std::vector<std::string> lines;
  lines.reserve(m_watches.size());
  for (const Watch& watch : m_watches)
    lines.push_back(fmt::format("{:x} {}", watch.address, watch.name));
  return lines;
}

void Watches::Clear()
{
  m_watches.clear();
}
}  // namespace Common::Debug
