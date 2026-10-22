// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/DSP/LabelMap.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Core/DSP/DSPTables.h"

namespace DSP
{
struct LabelMap::Label
{
  std::string name;
  u16 value;
  LabelType type;
};

LabelMap::LabelMap() = default;

LabelMap::~LabelMap() = default;

void LabelMap::RegisterDefaults()
{
  // Register names first, then the hardware registers; unnamed slots are skipped.
  const auto add_all = [this](const auto& table) {
    for (const pdlabel_t& entry : table)
    {
      if (entry.name != nullptr)
        RegisterLabel(entry.name, entry.addr);
    }
  };
  add_all(regnames);
  add_all(pdlabels);
}

bool LabelMap::RegisterLabel(std::string label, u16 lval, LabelType type)
{
  if (const std::optional<u16> existing = GetLabelValue(label))
  {
    if (*existing == lval)
      return true;

    // Note the argument order: the new value is printed first.
    fmt::print("Attempted to redefine label {} from {:04x} to {:04x}\n", label, lval, *existing);
    return false;
  }

  m_entries.push_back(Label{std::move(label), lval, type});
  return true;
}

void LabelMap::DeleteLabel(std::string_view label)
{
  const auto it = std::ranges::find_if(m_entries,
                                       [label](const Label& entry) { return entry.name == label; });
  if (it != m_entries.end())
    m_entries.erase(it);
}

std::optional<u16> LabelMap::GetLabelValue(std::string_view name, LabelType type) const
{
  // Several entries may share a name; the first one of an acceptable kind wins and
  // every unacceptable one on the way is reported.
  for (const Label& entry : m_entries)
  {
    if (entry.name != name)
      continue;
    if ((entry.type & type) != 0)
      return entry.value;
    fmt::print("Wrong label type requested. {}\n", name);
  }
  return std::nullopt;
}

void LabelMap::Clear()
{
  m_entries.clear();
}
}  // namespace DSP
