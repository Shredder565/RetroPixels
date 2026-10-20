// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Quantised paired-single loads and stores (psq_l / psq_st and their update and indexed
// forms).
//
// A GQR register chosen by the instruction's I field describes the memory format of each
// direction: a type (float, u8, u16, s8, s16) and a 6-bit signed scale. Loads multiply the
// integer by 2^-scale, stores multiply by 2^scale and saturate. With W set only one element is
// transferred and a load fills ps1 with 1.0.

#include "Core/PowerPC/Interpreter/Interpreter.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <type_traits>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Core/PowerPC/Interpreter/ExceptionUtils.h"
#include "Core/PowerPC/Interpreter/Interpreter_FPUtils.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
using PowerPC::PowerPCState;

// The scale field is a two's complement 6-bit number.
constexpr int ScaleExponent(u32 field)
{
  return field < 32 ? static_cast<int>(field) : static_cast<int>(field) - 64;
}

constexpr float PowerOfTwo(int exponent)
{
  float value = 1.0f;
  for (; exponent > 0; --exponent)
    value *= 2.0f;
  for (; exponent < 0; ++exponent)
    value *= 0.5f;
  return value;
}

// sign < 0 builds the load (dequantise) factors, sign > 0 the store (quantise) factors.
constexpr std::array<float, 64> MakeScaleTable(int sign)
{
  std::array<float, 64> table{};
  for (u32 field = 0; field < 64; ++field)
    table[field] = PowerOfTwo(sign * ScaleExponent(field));
  return table;
}

constexpr std::array<float, 64> LOAD_SCALE = MakeScaleTable(-1);
constexpr std::array<float, 64> STORE_SCALE = MakeScaleTable(1);

// Reads one element, or two adjacent elements as a single wider access.
template <typename U>
void ReadElements(PowerPC::MMU& mmu, u32 address, bool single, U& first, U& second)
{
  constexpr unsigned BITS = sizeof(U) * 8;
  using Wide = std::conditional_t<sizeof(U) == 1, u16, std::conditional_t<sizeof(U) == 2, u32, u64>>;
  if (single)
  {
    first = mmu.Read<U>(address);
    return;
  }
  const Wide both = mmu.Read<Wide>(address);
  first = static_cast<U>(both >> BITS);
  second = static_cast<U>(both);
}

template <typename U>
void WriteElements(PowerPC::MMU& mmu, u32 address, bool single, U first, U second)
{
  constexpr unsigned BITS = sizeof(U) * 8;
  using Wide = std::conditional_t<sizeof(U) == 1, u16, std::conditional_t<sizeof(U) == 2, u32, u64>>;
  if (single)
  {
    mmu.Write<U>(first, address);
    return;
  }
  mmu.Write<Wide>(static_cast<Wide>((static_cast<Wide>(first) << BITS) | second), address);
}

// ---- loads

template <typename T>
void LoadInteger(PowerPC::MMU& mmu, u32 address, bool single, u32 scale, double& ps0, double& ps1)
{
  using U = std::make_unsigned_t<T>;
  U first{}, second{};
  ReadElements<U>(mmu, address, single, first, second);
  const float factor = LOAD_SCALE[scale];
  // Products of small integers and powers of two are exact normal floats.
  const float f0 = static_cast<float>(static_cast<T>(first)) * factor;
  const float f1 = single ? 1.0f : static_cast<float>(static_cast<T>(second)) * factor;
  ps0 = f0;
  ps1 = f1;
}

void LoadQuantized(PowerPC::MMU& mmu, PowerPCState& s, u32 address, u32 gqr_index, u32 rd,
                   bool single)
{
  const UGQR gqr(s.spr[SPR_GQR0 + gqr_index]);
  const u32 scale = gqr.ld_scale;
  double ps0 = 0.0;
  double ps1 = 0.0;

  switch (gqr.ld_type)
  {
  case QUANTIZE_FLOAT:
  {
    u32 first = 0, second = 0;
    ReadElements<u32>(mmu, address, single, first, second);
    ps0 = std::bit_cast<double>(InterpreterFP::SingleBitsToDouble(first));
    ps1 = single ? 1.0 : std::bit_cast<double>(InterpreterFP::SingleBitsToDouble(second));
    break;
  }
  case QUANTIZE_U8:
    LoadInteger<u8>(mmu, address, single, scale, ps0, ps1);
    break;
  case QUANTIZE_U16:
    LoadInteger<u16>(mmu, address, single, scale, ps0, ps1);
    break;
  case QUANTIZE_S8:
    LoadInteger<s8>(mmu, address, single, scale, ps0, ps1);
    break;
  case QUANTIZE_S16:
    LoadInteger<s16>(mmu, address, single, scale, ps0, ps1);
    break;
  default:
    // Reserved types: nothing is read and the register becomes (0, 0).
    ASSERT_MSG(POWERPC, 0, "psq load with a reserved GQR type");
    break;
  }

  if (s.Exceptions & EXCEPTION_DSI)
    return;
  s.ps[rd].SetBoth(ps0, ps1);
}

// ---- stores

// Scales in single precision, saturates to the integer range and truncates.
template <typename T>
T Saturate(double value, u32 scale)
{
  constexpr float lowest = static_cast<float>(std::numeric_limits<T>::min());
  constexpr float highest = static_cast<float>(std::numeric_limits<T>::max());
  const float scaled = static_cast<float>(value) * STORE_SCALE[scale];
  return static_cast<T>(std::clamp(scaled, lowest, highest));
}

template <typename T>
void StoreInteger(PowerPC::MMU& mmu, u32 address, bool single, u32 scale, double ps0, double ps1)
{
  using U = std::make_unsigned_t<T>;
  const U first = static_cast<U>(Saturate<T>(ps0, scale));
  const U second = single ? U{0} : static_cast<U>(Saturate<T>(ps1, scale));
  WriteElements<U>(mmu, address, single, first, second);
}

void StoreQuantized(PowerPC::MMU& mmu, const PowerPCState& s, u32 address, u32 gqr_index, u32 rs,
                    bool single)
{
  const UGQR gqr(s.spr[SPR_GQR0 + gqr_index]);
  const u32 scale = gqr.st_scale;
  const double ps0 = s.ps[rs].PS0AsDouble();
  const double ps1 = s.ps[rs].PS1AsDouble();

  switch (gqr.st_type)
  {
  case QUANTIZE_FLOAT:
  {
    const u32 first = InterpreterFP::DoubleToSingleBitsFlushed(std::bit_cast<u64>(ps0));
    const u32 second =
        single ? 0u : InterpreterFP::DoubleToSingleBitsFlushed(std::bit_cast<u64>(ps1));
    WriteElements<u32>(mmu, address, single, first, second);
    break;
  }
  case QUANTIZE_U8:
    StoreInteger<u8>(mmu, address, single, scale, ps0, ps1);
    break;
  case QUANTIZE_U16:
    StoreInteger<u16>(mmu, address, single, scale, ps0, ps1);
    break;
  case QUANTIZE_S8:
    StoreInteger<s8>(mmu, address, single, scale, ps0, ps1);
    break;
  case QUANTIZE_S16:
    StoreInteger<s16>(mmu, address, single, scale, ps0, ps1);
    break;
  default:
    // Reserved types store nothing.
    ASSERT_MSG(POWERPC, 0, "psq store with a reserved GQR type");
    break;
  }
}

// The immediate forms are only legal while HID2[LSQE] is set.
bool QuantizedOpsEnabled(PowerPCState& s)
{
  if (HID2(s).LSQE)
    return true;
  GenerateProgramException(s, ProgramExceptionCause::IllegalInstruction);
  return false;
}

u32 ImmediateAddress(const PowerPCState& s, UGeckoInstruction inst, bool update)
{
  const u32 base = (update || inst.RA) ? s.gpr[inst.RA] : 0;
  return base + static_cast<u32>(inst.SIMM_12);
}

u32 IndexedAddress(const PowerPCState& s, UGeckoInstruction inst, bool update)
{
  const u32 base = (update || inst.RA) ? s.gpr[inst.RA] : 0;
  return base + s.gpr[inst.RB];
}

void UpdateBase(PowerPCState& s, UGeckoInstruction inst, u32 address)
{
  if (!(s.Exceptions & EXCEPTION_DSI))
    s.gpr[inst.RA] = address;
}
}  // namespace

void Interpreter::psq_l(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  if (!QuantizedOpsEnabled(s))
    return;
  LoadQuantized(interpreter.m_mmu, s, ImmediateAddress(s, inst, false), inst.I, inst.RD, inst.W);
}

void Interpreter::psq_lu(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  if (!QuantizedOpsEnabled(s))
    return;
  const u32 address = ImmediateAddress(s, inst, true);
  LoadQuantized(interpreter.m_mmu, s, address, inst.I, inst.RD, inst.W);
  UpdateBase(s, inst, address);
}

void Interpreter::psq_st(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  if (!QuantizedOpsEnabled(s))
    return;
  StoreQuantized(interpreter.m_mmu, s, ImmediateAddress(s, inst, false), inst.I, inst.RS, inst.W);
}

void Interpreter::psq_stu(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  if (!QuantizedOpsEnabled(s))
    return;
  const u32 address = ImmediateAddress(s, inst, true);
  StoreQuantized(interpreter.m_mmu, s, address, inst.I, inst.RS, inst.W);
  UpdateBase(s, inst, address);
}

// The indexed forms live in primary opcode 4 and are not gated by LSQE.
void Interpreter::psq_lx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  LoadQuantized(interpreter.m_mmu, s, IndexedAddress(s, inst, false), inst.Ix, inst.RD, inst.Wx);
}

void Interpreter::psq_lux(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  const u32 address = IndexedAddress(s, inst, true);
  LoadQuantized(interpreter.m_mmu, s, address, inst.Ix, inst.RD, inst.Wx);
  UpdateBase(s, inst, address);
}

void Interpreter::psq_stx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  StoreQuantized(interpreter.m_mmu, s, IndexedAddress(s, inst, false), inst.Ix, inst.RS, inst.Wx);
}

void Interpreter::psq_stux(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  const u32 address = IndexedAddress(s, inst, true);
  StoreQuantized(interpreter.m_mmu, s, address, inst.Ix, inst.RS, inst.Wx);
  UpdateBase(s, inst, address);
}
