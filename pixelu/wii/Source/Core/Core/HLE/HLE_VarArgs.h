// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <type_traits>

#include "Common/Align.h"
#include "Common/CommonTypes.h"

#include "Core/PowerPC/MMU.h"

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace HLE::SystemVABI
{
// Argument classes of the 32-bit PowerPC System V ABI (SVR4). Each class is
// fetched differently from registers or the stack.
template <typename T>
concept ArgPointer = std::is_union_v<T> || std::is_class_v<T>;
template <typename T>
concept ArgWord = std::is_pointer_v<T> || (std::is_integral_v<T> && sizeof(T) <= 4);
template <typename T>
concept ArgDoubleWord = std::is_integral_v<T> && sizeof(T) == 8;
template <typename T>
concept ArgReal = std::is_floating_point_v<T>;

// Walks the arguments of a guest function that was called with a variable
// argument list, following the SVR4 PowerPC parameter passing rules:
// integer words go in r3..r10, doubles in f1..f8, and whatever does not fit
// spills to the caller's parameter area on the stack.
//
// Reference: "System V Application Binary Interface, PowerPC Processor
// Supplement", sections "Parameter Passing" and "Variable Argument Lists".
// http://refspecs.linux-foundation.org/elf/elfspec_ppc.pdf
class VAList
{
public:
  explicit VAList(const Core::CPUThreadGuard& guard, u32 stack, u32 gpr = 3, u32 fpr = 1,
                  u32 gpr_max = 10, u32 fpr_max = 8)
      : m_guard(guard), m_gpr(gpr), m_fpr(fpr), m_gpr_max(gpr_max), m_fpr_max(fpr_max),
        m_stack(stack)
  {
  }
  virtual ~VAList();

  // Aggregates are passed by reference: the next word is their address, and
  // the object is copied out of guest memory byte by byte.
  template <ArgPointer T> T GetArg()
  {
    T obj;
    const u32 source = GetArg<u32>();
    auto* const bytes = reinterpret_cast<u8*>(&obj);
    for (u32 offset = 0; offset < sizeof(T); ++offset)
      bytes[offset] = PowerPC::MMU::HostRead<u8>(m_guard, source + offset);
    return obj;
  }

  // Anything up to 32 bits takes one GPR, or one aligned stack word.
  template <ArgWord T> T GetArg()
  {
    static_assert(!std::is_pointer<T>(), "VAList doesn't support pointers");
    const u64 raw = m_gpr <= m_gpr_max ? GetGPR(m_gpr++) : PopStack32();
    return static_cast<T>(raw);
  }

  // 64-bit integers take an odd/even register pair (r3:r4, r5:r6, ...), or
  // an 8-byte aligned stack slot once the pairs run out.
  template <ArgDoubleWord T> T GetArg()
  {
    m_gpr += (m_gpr & 1) ^ 1;
    if (m_gpr >= m_gpr_max)
      return static_cast<T>(PopStack64());

    const u64 high = GetGPR(m_gpr);
    const u64 low = GetGPR(m_gpr + 1);
    m_gpr += 2;
    return static_cast<T>((high << 32) | low);
  }

  // Floating point values are promoted to double and take one FPR.
  template <ArgReal T> T GetArg()
  {
    double raw;
    if (m_fpr <= m_fpr_max)
    {
      raw = GetFPR(m_fpr++);
    }
    else
    {
      m_stack = Common::AlignUp(m_stack, 8);
      raw = PowerPC::MMU::HostRead<double>(m_guard, m_stack);
      m_stack += 8;
    }
    return static_cast<T>(raw);
  }

  // Same as GetArg, with the result converted to T.
  template <typename T> T GetArgT() { return static_cast<T>(GetArg<T>()); }

protected:
  const Core::CPUThreadGuard& m_guard;
  u32 m_gpr = 3;
  u32 m_fpr = 1;
  const u32 m_gpr_max = 10;
  const u32 m_fpr_max = 8;
  u32 m_stack;

private:
  u32 PopStack32()
  {
    m_stack = Common::AlignUp(m_stack, 4);
    const u32 value = PowerPC::MMU::HostRead<u32>(m_guard, m_stack);
    m_stack += 4;
    return value;
  }

  u64 PopStack64()
  {
    m_stack = Common::AlignUp(m_stack, 8);
    const u64 value = PowerPC::MMU::HostRead<u64>(m_guard, m_stack);
    m_stack += 8;
    return value;
  }

  virtual u32 GetGPR(u32 gpr) const;
  virtual double GetFPR(u32 fpr) const;
};

// Arguments reached through a guest va_list object (as passed to vprintf and
// friends) rather than through the live registers. The object records how
// many registers were already consumed, where the spilled registers were
// saved, and where the stack overflow area begins.
class VAListStruct : public VAList
{
public:
  explicit VAListStruct(const Core::CPUThreadGuard& guard, u32 address);
  ~VAListStruct() override = default;

private:
  // Guest layout of va_list.
  struct svr4_va_list
  {
    u8 gpr;
    u8 fpr;
    u32 overflow_arg_area;
    u32 reg_save_area;
  };
  const svr4_va_list m_va_list;
  const u32 m_address;
  const bool m_has_fpr_area;

  u32 GetGPRArea() const;
  u32 GetFPRArea() const;

  u32 GetGPR(u32 gpr) const override;
  double GetFPR(u32 fpr) const override;
};

}  // namespace HLE::SystemVABI
