// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/ConditionRegister.h"

#include <cstddef>
#include <utility>

namespace PowerPC
{
namespace
{
template <std::size_t... I>
constexpr std::array<u64, 16> BuildFieldTable(std::index_sequence<I...>)
{
  return {ConditionRegister::PPCToInternal(static_cast<u8>(I))...};
}
}  // namespace

const std::array<u64, 16> ConditionRegister::s_crTable =
    BuildFieldTable(std::make_index_sequence<16>());

u32 ConditionRegister::Get() const
{
  u32 cr = 0;
  for (u32 field = 0; field < 8; ++field)
    cr = (cr << 4) | GetField(field);
  return cr;
}

void ConditionRegister::Set(u32 cr)
{
  for (u32 field = 8; field-- > 0;)
  {
    SetField(field, cr & 0xF);
    cr >>= 4;
  }
}

}  // namespace PowerPC
