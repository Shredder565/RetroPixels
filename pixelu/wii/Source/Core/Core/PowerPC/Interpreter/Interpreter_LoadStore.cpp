// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Integer and floating point loads/stores, string and multiple-word transfers, reservations,
// cache and TLB management.
//
// Conventions shared by every access: the MMU raises DSI itself by setting EXCEPTION_DSI; a load
// then leaves its target register alone and an update form leaves rA alone. When an update form
// names the same register as target and base, the base (written last) wins.

#include "Core/PowerPC/Interpreter/Interpreter.h"

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/PowerPC/Interpreter/ExceptionUtils.h"
#include "Core/PowerPC/Interpreter/Interpreter_FPUtils.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
using PowerPC::PowerPCState;

bool DSIRaised(const PowerPCState& ppc_state)
{
  return (ppc_state.Exceptions & EXCEPTION_DSI) != 0;
}

// Effective addresses. "Base" forms treat rA = 0 as the value 0; update forms always use rA.
u32 AddrD(const PowerPCState& s, UGeckoInstruction inst)
{
  return (inst.RA ? s.gpr[inst.RA] : 0) + static_cast<u32>(inst.SIMM_16);
}
u32 AddrDU(const PowerPCState& s, UGeckoInstruction inst)
{
  return s.gpr[inst.RA] + static_cast<u32>(inst.SIMM_16);
}
u32 AddrX(const PowerPCState& s, UGeckoInstruction inst)
{
  return (inst.RA ? s.gpr[inst.RA] : 0) + s.gpr[inst.RB];
}
u32 AddrXU(const PowerPCState& s, UGeckoInstruction inst)
{
  return s.gpr[inst.RA] + s.gpr[inst.RB];
}

enum class Ext
{
  Zero,
  Sign,
  Reverse,
};

template <typename T, Ext ext>
u32 LoadValue(PowerPC::MMU& mmu, u32 address)
{
  const T raw = mmu.Read<T>(address);
  if constexpr (ext == Ext::Sign)
    return static_cast<u32>(static_cast<s32>(static_cast<std::make_signed_t<T>>(raw)));
  else if constexpr (ext == Ext::Reverse)
  {
    if constexpr (sizeof(T) == 2)
      return Common::swap16(raw);
    else
      return Common::swap32(raw);
  }
  else
    return raw;
}

// Integer load into rD, optionally writing the address back to rA.
template <typename T, Ext ext = Ext::Zero>
void LoadInt(Interpreter& interpreter, PowerPCState& s, PowerPC::MMU& mmu, UGeckoInstruction inst,
             u32 address, bool update)
{
  const u32 value = LoadValue<T, ext>(mmu, address);
  if (DSIRaised(s))
    return;
  s.gpr[inst.RD] = value;
  if (update)
    s.gpr[inst.RA] = address;
}

// Integer store of rS, optionally writing the address back to rA.
template <typename T>
void StoreInt(PowerPCState& s, PowerPC::MMU& mmu, UGeckoInstruction inst, u32 address,
              bool update)
{
  mmu.Write<T>(s.gpr[inst.RS], address);
  if (update && !DSIRaised(s))
    s.gpr[inst.RA] = address;
}

// Floating point accesses must be word aligned.
bool CheckFloatAlignment(PowerPCState& s, u32 address)
{
  if ((address & 3) == 0)
    return true;
  GenerateAlignmentException(s, address);
  return false;
}

void LoadSingle(PowerPCState& s, PowerPC::MMU& mmu, UGeckoInstruction inst, u32 address,
                bool update)
{
  if (!CheckFloatAlignment(s, address))
    return;
  const u32 word = mmu.Read<u32>(address);
  if (DSIRaised(s))
    return;
  s.ps[inst.FD].Fill(InterpreterFP::SingleBitsToDouble(word));
  if (update)
    s.gpr[inst.RA] = address;
}

void LoadDouble(PowerPCState& s, PowerPC::MMU& mmu, UGeckoInstruction inst, u32 address,
                bool update)
{
  if (!CheckFloatAlignment(s, address))
    return;
  const u64 value = mmu.Read<u64>(address);
  if (DSIRaised(s))
    return;
  s.ps[inst.FD].SetPS0(value);
  if (update)
    s.gpr[inst.RA] = address;
}

void StoreSingle(PowerPCState& s, PowerPC::MMU& mmu, UGeckoInstruction inst, u32 address,
                 bool update)
{
  if (!CheckFloatAlignment(s, address))
    return;
  mmu.Write<u32>(InterpreterFP::DoubleToSingleBits(s.ps[inst.FS].PS0AsU64()), address);
  if (update && !DSIRaised(s))
    s.gpr[inst.RA] = address;
}

void StoreDouble(PowerPCState& s, PowerPC::MMU& mmu, UGeckoInstruction inst, u32 address,
                 bool update)
{
  if (!CheckFloatAlignment(s, address))
    return;
  mmu.Write<u64>(s.ps[inst.FS].PS0AsU64(), address);
  if (update && !DSIRaised(s))
    s.gpr[inst.RA] = address;
}

// lswi/lswx: `count` bytes into consecutive registers (wrapping after r31), four per register,
// big-endian; each register is cleared when its first byte arrives. A count of zero changes
// nothing (verified on hardware).
void LoadString(PowerPCState& s, PowerPC::MMU& mmu, u32 address, u32 count, u32 first_reg,
                bool alert_on_dsi)
{
  for (u32 n = 0; n < count; n++)
  {
    const u32 reg = (first_reg + n / 4) & 31;
    const u32 shift = 24 - 8 * (n % 4);
    if (n % 4 == 0)
      s.gpr[reg] = 0;
    const u32 byte = mmu.Read<u8>(address + n);
    if (DSIRaised(s))
    {
      if (alert_on_dsi)
        PanicAlertFmt("lswi: DSI exception during the transfer");
      NOTICE_LOG_FMT(POWERPC, "DSI exception during a load string instruction");
      return;
    }
    s.gpr[reg] |= byte << shift;
  }
}

// stswi/stswx: `count` bytes from consecutive registers. Uncached memory must not see accesses
// narrower or wider than the instruction asks for, so the bytes go out as aligned 32-bit words;
// partial words at either end are merged with the bytes already in memory.
void StoreString(PowerPCState& s, PowerPC::MMU& mmu, u32 address, u32 count, u32 first_reg)
{
  const u32 lead = address & 3;  // bytes of the first word that are not ours
  u32 word_address = address & ~3u;
  const u32 total = lead + count;

  // Byte `i` of the stream that will be written from word_address onwards.
  u32 head_word = 0;
  if (lead != 0)
  {
    head_word = mmu.Read<u32>(word_address);
    if (DSIRaised(s))
      return;
  }
  auto data_byte = [&](u32 i) -> u32 {
    const u32 k = i - lead;
    return (s.gpr[(first_reg + k / 4) & 31] >> (24 - 8 * (k % 4))) & 0xFF;
  };
  auto head_byte = [&](u32 i) -> u32 { return (head_word >> (24 - 8 * i)) & 0xFF; };

  u32 i = 0;
  for (; i + 4 <= total; i += 4, word_address += 4)
  {
    u32 word = 0;
    for (u32 j = 0; j < 4; j++)
      word = (word << 8) | (i + j < lead ? head_byte(i + j) : data_byte(i + j));
    mmu.Write<u32>(word, word_address);
    if (DSIRaised(s))
      return;
  }

  const u32 remaining = total - i;
  if (remaining == 0)
    return;

  const u32 tail_word = mmu.Read<u32>(word_address);
  if (DSIRaised(s))
    return;
  u32 word = 0;
  for (u32 j = 0; j < 4; j++)
  {
    u32 byte;
    if (j >= remaining)
      byte = (tail_word >> (24 - 8 * j)) & 0xFF;
    else if (i + j < lead)
      byte = head_byte(i + j);
    else
      byte = data_byte(i + j);
    word = (word << 8) | byte;
  }
  mmu.Write<u32>(word, word_address);
}

// Without data cache emulation the cache block instructions instead invalidate the JIT's view
// of the line: games that patch code tend to flush the data cache but not always icbi.
template <typename Op>
void DataCacheOp(Interpreter& interpreter, PowerPCState& s, Core::System& system, u32 address,
                 Op op)
{
  if (!s.m_enable_dcache)
  {
    system.GetJitInterface().InvalidateICacheLine(address);
    return;
  }
  op(address);
}

// eciwx/ecowx need EAR[E] and word alignment.
bool ExternalControlAllowed(PowerPCState& s, u32 address)
{
  if ((s.spr[SPR_EAR] & 0x80000000) == 0)
  {
    GenerateDSIException(s, address);
    return false;
  }
  if ((address & 3) != 0)
  {
    GenerateAlignmentException(s, address);
    return false;
  }
  return true;
}

bool Privileged(PowerPCState& s)
{
  if (!s.msr.PR)
    return false;
  GenerateProgramException(s, ProgramExceptionCause::PrivilegedInstruction);
  return true;
}
}  // namespace

// ---- integer loads

void Interpreter::lbz(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u8>(i, i.m_ppc_state, i.m_mmu, inst, AddrD(i.m_ppc_state, inst), false);
}
void Interpreter::lbzu(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u8>(i, i.m_ppc_state, i.m_mmu, inst, AddrDU(i.m_ppc_state, inst), true);
}
void Interpreter::lbzx(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u8>(i, i.m_ppc_state, i.m_mmu, inst, AddrX(i.m_ppc_state, inst), false);
}
void Interpreter::lbzux(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u8>(i, i.m_ppc_state, i.m_mmu, inst, AddrXU(i.m_ppc_state, inst), true);
}

void Interpreter::lhz(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u16>(i, i.m_ppc_state, i.m_mmu, inst, AddrD(i.m_ppc_state, inst), false);
}
void Interpreter::lhzu(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u16>(i, i.m_ppc_state, i.m_mmu, inst, AddrDU(i.m_ppc_state, inst), true);
}
void Interpreter::lhzx(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u16>(i, i.m_ppc_state, i.m_mmu, inst, AddrX(i.m_ppc_state, inst), false);
}
void Interpreter::lhzux(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u16>(i, i.m_ppc_state, i.m_mmu, inst, AddrXU(i.m_ppc_state, inst), true);
}

void Interpreter::lha(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u16, Ext::Sign>(i, i.m_ppc_state, i.m_mmu, inst, AddrD(i.m_ppc_state, inst), false);
}
void Interpreter::lhau(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u16, Ext::Sign>(i, i.m_ppc_state, i.m_mmu, inst, AddrDU(i.m_ppc_state, inst), true);
}
void Interpreter::lhax(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u16, Ext::Sign>(i, i.m_ppc_state, i.m_mmu, inst, AddrX(i.m_ppc_state, inst), false);
}
void Interpreter::lhaux(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u16, Ext::Sign>(i, i.m_ppc_state, i.m_mmu, inst, AddrXU(i.m_ppc_state, inst), true);
}

void Interpreter::lwz(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u32>(i, i.m_ppc_state, i.m_mmu, inst, AddrD(i.m_ppc_state, inst), false);
}
void Interpreter::lwzu(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u32>(i, i.m_ppc_state, i.m_mmu, inst, AddrDU(i.m_ppc_state, inst), true);
}
void Interpreter::lwzx(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u32>(i, i.m_ppc_state, i.m_mmu, inst, AddrX(i.m_ppc_state, inst), false);
}
void Interpreter::lwzux(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u32>(i, i.m_ppc_state, i.m_mmu, inst, AddrXU(i.m_ppc_state, inst), true);
}

void Interpreter::lhbrx(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u16, Ext::Reverse>(i, i.m_ppc_state, i.m_mmu, inst, AddrX(i.m_ppc_state, inst), false);
}
void Interpreter::lwbrx(Interpreter& i, UGeckoInstruction inst)
{
  LoadInt<u32, Ext::Reverse>(i, i.m_ppc_state, i.m_mmu, inst, AddrX(i.m_ppc_state, inst), false);
}

// ---- integer stores

void Interpreter::stb(Interpreter& i, UGeckoInstruction inst)
{
  StoreInt<u8>(i.m_ppc_state, i.m_mmu, inst, AddrD(i.m_ppc_state, inst), false);
}
void Interpreter::stbu(Interpreter& i, UGeckoInstruction inst)
{
  StoreInt<u8>(i.m_ppc_state, i.m_mmu, inst, AddrDU(i.m_ppc_state, inst), true);
}
void Interpreter::stbx(Interpreter& i, UGeckoInstruction inst)
{
  StoreInt<u8>(i.m_ppc_state, i.m_mmu, inst, AddrX(i.m_ppc_state, inst), false);
}
void Interpreter::stbux(Interpreter& i, UGeckoInstruction inst)
{
  StoreInt<u8>(i.m_ppc_state, i.m_mmu, inst, AddrXU(i.m_ppc_state, inst), true);
}

void Interpreter::sth(Interpreter& i, UGeckoInstruction inst)
{
  StoreInt<u16>(i.m_ppc_state, i.m_mmu, inst, AddrD(i.m_ppc_state, inst), false);
}
void Interpreter::sthu(Interpreter& i, UGeckoInstruction inst)
{
  StoreInt<u16>(i.m_ppc_state, i.m_mmu, inst, AddrDU(i.m_ppc_state, inst), true);
}
void Interpreter::sthx(Interpreter& i, UGeckoInstruction inst)
{
  StoreInt<u16>(i.m_ppc_state, i.m_mmu, inst, AddrX(i.m_ppc_state, inst), false);
}
void Interpreter::sthux(Interpreter& i, UGeckoInstruction inst)
{
  StoreInt<u16>(i.m_ppc_state, i.m_mmu, inst, AddrXU(i.m_ppc_state, inst), true);
}

void Interpreter::stw(Interpreter& i, UGeckoInstruction inst)
{
  StoreInt<u32>(i.m_ppc_state, i.m_mmu, inst, AddrD(i.m_ppc_state, inst), false);
}
void Interpreter::stwu(Interpreter& i, UGeckoInstruction inst)
{
  StoreInt<u32>(i.m_ppc_state, i.m_mmu, inst, AddrDU(i.m_ppc_state, inst), true);
}
void Interpreter::stwx(Interpreter& i, UGeckoInstruction inst)
{
  StoreInt<u32>(i.m_ppc_state, i.m_mmu, inst, AddrX(i.m_ppc_state, inst), false);
}
void Interpreter::stwux(Interpreter& i, UGeckoInstruction inst)
{
  StoreInt<u32>(i.m_ppc_state, i.m_mmu, inst, AddrXU(i.m_ppc_state, inst), true);
}

void Interpreter::sthbrx(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  i.m_mmu.Write_U16_Swap(s.gpr[inst.RS], AddrX(s, inst));
}
void Interpreter::stwbrx(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  i.m_mmu.Write_U32_Swap(s.gpr[inst.RS], AddrX(s, inst));
}

// ---- floating point loads and stores

void Interpreter::lfs(Interpreter& i, UGeckoInstruction inst)
{
  LoadSingle(i.m_ppc_state, i.m_mmu, inst, AddrD(i.m_ppc_state, inst), false);
}
void Interpreter::lfsu(Interpreter& i, UGeckoInstruction inst)
{
  LoadSingle(i.m_ppc_state, i.m_mmu, inst, AddrDU(i.m_ppc_state, inst), true);
}
void Interpreter::lfsx(Interpreter& i, UGeckoInstruction inst)
{
  LoadSingle(i.m_ppc_state, i.m_mmu, inst, AddrX(i.m_ppc_state, inst), false);
}
void Interpreter::lfsux(Interpreter& i, UGeckoInstruction inst)
{
  LoadSingle(i.m_ppc_state, i.m_mmu, inst, AddrXU(i.m_ppc_state, inst), true);
}

void Interpreter::lfd(Interpreter& i, UGeckoInstruction inst)
{
  LoadDouble(i.m_ppc_state, i.m_mmu, inst, AddrD(i.m_ppc_state, inst), false);
}
void Interpreter::lfdu(Interpreter& i, UGeckoInstruction inst)
{
  LoadDouble(i.m_ppc_state, i.m_mmu, inst, AddrDU(i.m_ppc_state, inst), true);
}
void Interpreter::lfdx(Interpreter& i, UGeckoInstruction inst)
{
  LoadDouble(i.m_ppc_state, i.m_mmu, inst, AddrX(i.m_ppc_state, inst), false);
}
void Interpreter::lfdux(Interpreter& i, UGeckoInstruction inst)
{
  LoadDouble(i.m_ppc_state, i.m_mmu, inst, AddrXU(i.m_ppc_state, inst), true);
}

void Interpreter::stfs(Interpreter& i, UGeckoInstruction inst)
{
  StoreSingle(i.m_ppc_state, i.m_mmu, inst, AddrD(i.m_ppc_state, inst), false);
}
void Interpreter::stfsu(Interpreter& i, UGeckoInstruction inst)
{
  StoreSingle(i.m_ppc_state, i.m_mmu, inst, AddrDU(i.m_ppc_state, inst), true);
}
void Interpreter::stfsx(Interpreter& i, UGeckoInstruction inst)
{
  StoreSingle(i.m_ppc_state, i.m_mmu, inst, AddrX(i.m_ppc_state, inst), false);
}
void Interpreter::stfsux(Interpreter& i, UGeckoInstruction inst)
{
  StoreSingle(i.m_ppc_state, i.m_mmu, inst, AddrXU(i.m_ppc_state, inst), true);
}

void Interpreter::stfd(Interpreter& i, UGeckoInstruction inst)
{
  StoreDouble(i.m_ppc_state, i.m_mmu, inst, AddrD(i.m_ppc_state, inst), false);
}
void Interpreter::stfdu(Interpreter& i, UGeckoInstruction inst)
{
  StoreDouble(i.m_ppc_state, i.m_mmu, inst, AddrDU(i.m_ppc_state, inst), true);
}
void Interpreter::stfdx(Interpreter& i, UGeckoInstruction inst)
{
  StoreDouble(i.m_ppc_state, i.m_mmu, inst, AddrX(i.m_ppc_state, inst), false);
}
void Interpreter::stfdux(Interpreter& i, UGeckoInstruction inst)
{
  StoreDouble(i.m_ppc_state, i.m_mmu, inst, AddrXU(i.m_ppc_state, inst), true);
}

// stfiwx: the low word of frS, as is.
void Interpreter::stfiwx(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  const u32 address = AddrX(s, inst);
  if (!CheckFloatAlignment(s, address))
    return;
  i.m_mmu.Write<u32>(s.ps[inst.FS].PS0AsU32(), address);
}

// ---- multiple and string transfers. A DSI in the middle leaves the registers or memory
// already transferred as they are (real hardware would restart the instruction).

void Interpreter::lmw(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  u32 address = AddrD(s, inst);
  if ((address & 3) != 0 || s.msr.LE)
  {
    GenerateAlignmentException(s, address);
    return;
  }
  for (u32 reg = inst.RD; reg < 32; reg++, address += 4)
  {
    const u32 value = i.m_mmu.Read<u32>(address);
    if (DSIRaised(s))
    {
      PanicAlertFmt("lmw: DSI exception during the transfer");
      NOTICE_LOG_FMT(POWERPC, "DSI exception in lmw");
      return;
    }
    s.gpr[reg] = value;
  }
}

void Interpreter::stmw(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  u32 address = AddrD(s, inst);
  if ((address & 3) != 0 || s.msr.LE)
  {
    GenerateAlignmentException(s, address);
    return;
  }
  for (u32 reg = inst.RS; reg < 32; reg++, address += 4)
  {
    i.m_mmu.Write<u32>(s.gpr[reg], address);
    if (DSIRaised(s))
    {
      PanicAlertFmt("stmw: DSI exception during the transfer");
      NOTICE_LOG_FMT(POWERPC, "DSI exception in stmw");
      return;
    }
  }
}

// The string instructions are not available in little-endian mode.
void Interpreter::lswi(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  const u32 address = inst.RA ? s.gpr[inst.RA] : 0;
  if (s.msr.LE)
  {
    GenerateAlignmentException(s, address);
    return;
  }
  LoadString(s, i.m_mmu, address, inst.NB == 0 ? 32 : inst.NB, inst.RD, true);
}

void Interpreter::lswx(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  const u32 address = AddrX(s, inst);
  if (s.msr.LE)
  {
    GenerateAlignmentException(s, address);
    return;
  }
  // XER[25-31] holds the byte count (Not64, a Wii homebrew N64 emulator, uses this).
  LoadString(s, i.m_mmu, address, static_cast<u8>(s.xer_stringctrl), inst.RD, false);
}

void Interpreter::stswi(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  const u32 address = inst.RA ? s.gpr[inst.RA] : 0;
  if (s.msr.LE)
  {
    GenerateAlignmentException(s, address);
    return;
  }
  StoreString(s, i.m_mmu, address, inst.NB == 0 ? 32 : inst.NB, inst.RS);
}

void Interpreter::stswx(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  const u32 address = AddrX(s, inst);
  if (s.msr.LE)
  {
    GenerateAlignmentException(s, address);
    return;
  }
  StoreString(s, i.m_mmu, address, static_cast<u8>(s.xer_stringctrl), inst.RS);
}

// ---- reservations. With a single CPU a reservation is only lost to another stwcx.

void Interpreter::lwarx(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  const u32 address = AddrX(s, inst);
  if (!CheckFloatAlignment(s, address))
    return;
  const u32 value = i.m_mmu.Read<u32>(address);
  if (DSIRaised(s))
    return;
  s.gpr[inst.RD] = value;
  s.reserve = true;
  s.reserve_address = address;
}

// stwcx.: CR0 = 00 || stored || SO.
void Interpreter::stwcxd(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  const u32 address = AddrX(s, inst);
  if (!CheckFloatAlignment(s, address))
    return;

  if (s.reserve && s.reserve_address == address)
  {
    i.m_mmu.Write<u32>(s.gpr[inst.RS], address);
    if (!DSIRaised(s))
    {
      s.reserve = false;
      s.cr.SetField(0, PowerPC::CR_EQ | s.GetXER_SO());
      return;
    }
  }
  s.cr.SetField(0, s.GetXER_SO());
}

// ---- external control (unused by games)

void Interpreter::eciwx(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  const u32 address = AddrX(s, inst);
  if (ExternalControlAllowed(s, address))
    s.gpr[inst.RD] = i.m_mmu.Read<u32>(address);
}

void Interpreter::ecowx(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  const u32 address = AddrX(s, inst);
  if (ExternalControlAllowed(s, address))
    i.m_mmu.Write<u32>(s.gpr[inst.RS], address);
}

// ---- cache management

void Interpreter::dcba(Interpreter& i, UGeckoInstruction inst)
{
  ASSERT_MSG(POWERPC, false, "dcba is not a Gekko instruction");
}

void Interpreter::dcbf(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  DataCacheOp(i, s, i.m_system, AddrX(s, inst),
              [&](u32 address) { i.m_mmu.FlushDCacheLine(address); });
}

void Interpreter::dcbi(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  if (Privileged(s))
    return;
  DataCacheOp(i, s, i.m_system, AddrX(s, inst),
              [&](u32 address) { i.m_mmu.InvalidateDCacheLine(address); });
}

void Interpreter::dcbst(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  DataCacheOp(i, s, i.m_system, AddrX(s, inst),
              [&](u32 address) { i.m_mmu.StoreDCacheLine(address); });
}

// Touch hints: prefetching is never required for correctness and is not worth emulating.
void Interpreter::dcbt(Interpreter& i, UGeckoInstruction inst)
{
}

void Interpreter::dcbtst(Interpreter& i, UGeckoInstruction inst)
{
}

// dcbz needs the data cache enabled (HID0[DCE]); without it the Gekko raises an alignment
// exception.
void Interpreter::dcbz(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  const u32 address = AddrX(s, inst);
  if (!HID0(s).DCE)
  {
    GenerateAlignmentException(s, address);
    return;
  }

  // Optional hack: ignore dcbz on the first 32 KiB of MEM1 (only without data cache emulation,
  // which would make it unnecessary).
  if (!s.m_enable_dcache && address >= 0x80000000 && address < 0x80008000 &&
      Config::Get(Config::MAIN_LOW_DCBZ_HACK))
  {
    return;
  }

  i.m_mmu.ClearDCacheLine(address & ~31u);
}

// dcbz_l zeroes a line of the locked cache and needs HID2[LCE].
void Interpreter::dcbz_l(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  if (!HID2(s).LCE)
  {
    GenerateProgramException(s, ProgramExceptionCause::IllegalInstruction);
    return;
  }
  const u32 address = AddrX(s, inst);
  if (!HID0(s).DCE)
  {
    GenerateAlignmentException(s, address);
    return;
  }
  i.m_mmu.ClearDCacheLine(address & ~31u);
}

void Interpreter::icbi(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  const u32 address = AddrX(s, inst);
  s.iCache.Invalidate(i.m_system.GetMemory(), i.m_system.GetJitInterface(), address);
}

// ---- ordering (everything already happens in program order here)

void Interpreter::eieio(Interpreter& i, UGeckoInstruction inst)
{
}

void Interpreter::sync(Interpreter& i, UGeckoInstruction inst)
{
}

// ---- TLB management

void Interpreter::tlbie(Interpreter& i, UGeckoInstruction inst)
{
  auto& s = i.m_ppc_state;
  if (Privileged(s))
    return;
  i.m_mmu.InvalidateTLBEntry(s.gpr[inst.RB]);
}

void Interpreter::tlbsync(Interpreter& i, UGeckoInstruction inst)
{
  Privileged(i.m_ppc_state);
}
