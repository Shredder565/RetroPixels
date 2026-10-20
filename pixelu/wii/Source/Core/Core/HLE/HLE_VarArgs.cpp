// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HLE/HLE_VarArgs.h"

#include "Common/Logging/Log.h"

#include "Core/Core.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace HLE::SystemVABI
{
namespace
{
// The register save area holds r3..r10 (4 bytes each) followed by f1..f8
// (8 bytes each).
constexpr u32 FIRST_ARG_GPR = 3;
constexpr u32 LAST_ARG_GPR = 10;
constexpr u32 FIRST_ARG_FPR = 1;
constexpr u32 LAST_ARG_FPR = 8;
constexpr u32 SAVED_GPR_BYTES = 4 * (LAST_ARG_GPR - FIRST_ARG_GPR + 1);

// CR bit 6 is set by the caller of a variadic function when floating point
// arguments were passed in registers, which makes the prologue save them.
constexpr u32 CR_BIT_FPR_ARGS = 6;

template <typename T> T GuestRead(const Core::CPUThreadGuard& guard, u32 base, u32 offset)
{
  return PowerPC::MMU::HostRead<T>(guard, base + offset);
}
}  // namespace

VAList::~VAList() = default;

u32 VAList::GetGPR(u32 gpr) const
{
  return m_guard.GetSystem().GetPPCState().gpr[gpr];
}

double VAList::GetFPR(u32 fpr) const
{
  return m_guard.GetSystem().GetPPCState().ps[fpr].PS0AsDouble();
}

VAListStruct::VAListStruct(const Core::CPUThreadGuard& guard, u32 address)
    : VAList(guard, 0),
      // Guest va_list: u8 gpr count, u8 fpr count, 2 bytes padding, then the
      // overflow area and register save area pointers.
      m_va_list{GuestRead<u8>(guard, address, 0), GuestRead<u8>(guard, address, 1),
                GuestRead<u32>(guard, address, 4), GuestRead<u32>(guard, address, 8)},
      m_address(address),
      m_has_fpr_area(guard.GetSystem().GetPPCState().cr.GetBit(CR_BIT_FPR_ARGS) == 1)
{
  // Resume where the va_list left off.
  m_gpr += m_va_list.gpr;
  m_fpr += m_va_list.fpr;
  m_stack = m_va_list.overflow_arg_area;
}

u32 VAListStruct::GetGPRArea() const
{
  return m_va_list.reg_save_area;
}

u32 VAListStruct::GetFPRArea() const
{
  return GetGPRArea() + SAVED_GPR_BYTES;
}

u32 VAListStruct::GetGPR(u32 gpr) const
{
  const bool saved = gpr >= FIRST_ARG_GPR && gpr <= LAST_ARG_GPR;
  if (!saved)
  {
    ERROR_LOG_FMT(OSHLE, "VAListStruct at {:08x} doesn't have GPR{}!", m_address, gpr);
    return 0;
  }
  const u32 slot = GetGPRArea() + (gpr - FIRST_ARG_GPR) * 4;
  return PowerPC::MMU::HostRead<u32>(m_guard, Common::AlignUp(slot, 4));
}

double VAListStruct::GetFPR(u32 fpr) const
{
  const bool saved = m_has_fpr_area && fpr >= FIRST_ARG_FPR && fpr <= LAST_ARG_FPR;
  if (!saved)
  {
    ERROR_LOG_FMT(OSHLE, "VAListStruct at {:08x} doesn't have FPR{}!", m_address, fpr);
    return 0.0;
  }
  const u32 slot = GetFPRArea() + (fpr - FIRST_ARG_FPR) * 8;
  return PowerPC::MMU::HostRead<double>(m_guard, Common::AlignUp(slot, 8));
}
}  // namespace HLE::SystemVABI
