// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The four hardware stacks (call, data, loop address, loop counter). The visible
// register $stN is the top of stack N; the entries below it live in reg_stacks.

#include "Core/DSP/DSPCore.h"

#include <cstddef>

#include "Common/CommonTypes.h"

namespace DSP
{
void SDSP::StoreStack(StackRegister stack_reg, u16 val)
{
  const auto which = static_cast<size_t>(stack_reg);
  u8& depth = reg_stack_ptrs[which];

  // Push the current top down, then make the new value the top.
  depth = static_cast<u8>((depth + 1) & DSP_STACK_MASK);
  reg_stacks[which][depth] = r.st[which];
  r.st[which] = val;
}

u16 SDSP::PopStack(StackRegister stack_reg)
{
  const auto which = static_cast<size_t>(stack_reg);
  u8& depth = reg_stack_ptrs[which];

  const u16 top = r.st[which];
  r.st[which] = reg_stacks[which][depth];
  depth = static_cast<u8>((depth - 1) & DSP_STACK_MASK);
  return top;
}
}  // namespace DSP
