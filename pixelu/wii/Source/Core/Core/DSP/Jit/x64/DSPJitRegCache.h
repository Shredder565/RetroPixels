// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The DSP register file as generated code sees it.
//
// The recompiler does not keep DSP registers in host registers from one
// instruction to the next: every register stays in the SDSP structure, which
// STATE_REG points at for as long as generated code runs. Each instruction
// loads what it needs into scratch registers, computes, and stores the results
// straight back. Calls into C++ (the interpreter, the hardware registers,
// exception checks) therefore never have to save or reload guest state.
//
// Host register use inside generated code:
//   STATE_REG (R14)         the SDSP
//   ENTRY_TABLE (R15)       the dispatcher's table of block entry points
//   CYCLE_COUNTER (RBX)     points at the cycles left in this run
//   EXT_FIRST/EXT_SECOND    values an extension opcode has loaded but not yet
//   (R12, R13)              written; they survive calls (callee saved)
//   RAX, RCX, RDX, RSI, RDI values inside one instruction
//   R8 - R11                scratch of the helpers (flags, stacks, memory,
//                           address registers, register stores)
// A memory access or any other call clobbers every caller-saved register, so
// instructions hold nothing in them across one.

#pragma once

#include "Common/CommonTypes.h"
#include "Common/x64Emitter.h"

namespace DSP
{
struct SDSP;
}

namespace DSP::JIT::x64
{
constexpr Gen::X64Reg STATE_REG = Gen::R14;
constexpr Gen::X64Reg ENTRY_TABLE = Gen::R15;
constexpr Gen::X64Reg CYCLE_COUNTER = Gen::RBX;
constexpr Gen::X64Reg EXT_FIRST = Gen::R12;
constexpr Gen::X64Reg EXT_SECOND = Gen::R13;

constexpr Gen::X64Reg HELPER_A = Gen::R8;
constexpr Gen::X64Reg HELPER_B = Gen::R9;
constexpr Gen::X64Reg HELPER_C = Gen::R10;
constexpr Gen::X64Reg HELPER_D = Gen::R11;

// How a 16-bit register is widened when it is loaded into a host register.
enum class RegisterExtension
{
  Sign,
  Zero,
  None,  // only the low 16 bits are written
};

class RegisterFile
{
public:
  RegisterFile(Gen::XEmitter& code, const SDSP& state);

  // Memory operands (relative to STATE_REG) of the guest state.
  Gen::OpArg Word(int reg) const;  // $0-$31; $acX.h is the low half of its 32-bit field
  Gen::OpArg Acc(int acc) const;   // 64 bits, the 40-bit value sign extended
  Gen::OpArg AccHigh(int acc) const;  // 32 bits
  Gen::OpArg Ax(int ax) const;        // 32 bits
  Gen::OpArg Product() const;         // 64 bits: l, m, h, m2
  Gen::OpArg Pc() const;
  Gen::OpArg ControlReg() const;
  Gen::OpArg Exceptions() const;
  Gen::OpArg ExternalInterruptWaiting() const;
  Gen::OpArg StackDepth(int stack) const;  // 8 bits
  Gen::OpArg StackTop(int stack) const;    // $st0-$st3
  // reg_stacks[stack][index] for an index (0-31) held in a 32-bit register.
  Gen::OpArg StackEntry(int stack, Gen::X64Reg index) const;

  // Plain loads of a register's 16 bits; no stack or saturation side effects.
  void LoadWord(int reg, Gen::X64Reg dst, RegisterExtension ext);

  // Plain stores of a register (not $st0-$st3) from the low 16 bits of src.
  // They keep the fixed bits the way the chip shows them: $acX.h holds 8 bits
  // sign extended, $cr and $prod.h hold 8 bits, bit 8 of $sr reads as 0.
  // src is left as it was. src must not be HELPER_D.
  void StoreWord(int reg, Gen::X64Reg src);
  void StoreWordImm(int reg, u16 value);

  // The 40-bit accumulators as sign-extended 64-bit values.
  void LoadAcc(int acc, Gen::X64Reg dst);
  // Stores the low 40 bits of src (sign extended); src is left as it was. Clobbers HELPER_D.
  void StoreAcc(int acc, Gen::X64Reg src);

  // $axX as a sign-extended 32-bit value.
  void LoadAx(int ax, Gen::X64Reg dst);

private:
  int AccOffset(int acc) const;

  Gen::XEmitter& m_code;

  // Byte offsets inside SDSP.
  int m_ar = 0;
  int m_ix = 0;
  int m_wr = 0;
  int m_st = 0;
  int m_cr = 0;
  int m_sr = 0;
  int m_prod = 0;
  int m_ax = 0;
  int m_ac = 0;
  int m_ac_stride = 0;
  int m_ax_stride = 0;
  int m_pc = 0;
  int m_control = 0;
  int m_exceptions = 0;
  int m_ext_irq = 0;
  int m_stack_depth = 0;
  int m_stacks = 0;
  int m_stack_stride = 0;
};

}  // namespace DSP::JIT::x64
