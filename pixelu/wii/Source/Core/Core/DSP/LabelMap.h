// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The symbol table of the DSP assembler: names bound to 16-bit values, each
// tagged with what kind of value it is.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace DSP
{
// A label's kind. These are bit flags so that a lookup can accept several.
enum LabelType
{
  LABEL_IADDR = 1,  // an instruction address (jump target)
  LABEL_DADDR = 2,  // a data address
  LABEL_VALUE = 4,  // a plain number
  LABEL_ANY = 0xFF,
};

class LabelMap
{
public:
  LabelMap();
  ~LabelMap();

  // Adds the register names and the hardware register addresses.
  void RegisterDefaults();

  // Binds label to lval. Binding a name again is accepted only with the same value.
  bool RegisterLabel(std::string label, u16 lval, LabelType type = LABEL_VALUE);
  void DeleteLabel(std::string_view label);
  std::optional<u16> GetLabelValue(std::string_view name, LabelType type = LABEL_ANY) const;
  void Clear();

private:
  struct Label;
  std::vector<Label> m_entries;
};
}  // namespace DSP
