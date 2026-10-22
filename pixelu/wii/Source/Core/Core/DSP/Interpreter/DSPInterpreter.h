// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The DSP interpreter. Besides running code itself it provides the handlers the
// JIT falls back to for instructions it does not compile; each handler is one
// instruction (or, for the extension handlers, one extension) and can be called
// on its own.

#pragma once

#include <array>
#include <cstddef>

#include "Core/DSP/DSPCommon.h"
#include "Core/DSP/DSPCore.h"

namespace DSP::Interpreter
{
class Interpreter
{
public:
  explicit Interpreter(DSPCore& dsp);
  ~Interpreter();

  Interpreter(const Interpreter&) = delete;
  Interpreter& operator=(const Interpreter&) = delete;

  Interpreter(Interpreter&&) = delete;
  Interpreter& operator=(Interpreter&&) = delete;

  // Executes one instruction at pc (after taking a pending exception, if any).
  void Step();

  // Run for about the given number of cycles; they return what is left (0 when the
  // DSP halted or went idle). RunCyclesDebug also stops at breakpoints.
  int RunCyclesThread(int cycles);
  int RunCycles(int cycles);
  int RunCyclesDebug(int cycles);

  // The control register shared with the CPU (reset, init, halt bits).
  void WriteControlRegister(u16 val);
  u16 ReadControlRegister();

  void SetSRFlag(u16 flag);
  bool IsSRFlagSet(u16 flag) const;

  // Commits the register writes an extension handler deferred.
  void ApplyWriteBackLog();

  // Main instruction handlers.
  void abs(UDSPInstruction opc);
  void add(UDSPInstruction opc);
  void addarn(UDSPInstruction opc);
  void addax(UDSPInstruction opc);
  void addaxl(UDSPInstruction opc);
  void addi(UDSPInstruction opc);
  void addis(UDSPInstruction opc);
  void addp(UDSPInstruction opc);
  void addpaxz(UDSPInstruction opc);
  void addr(UDSPInstruction opc);
  void andc(UDSPInstruction opc);
  void andcf(UDSPInstruction opc);
  void andf(UDSPInstruction opc);
  void andi(UDSPInstruction opc);
  void andr(UDSPInstruction opc);
  void asl(UDSPInstruction opc);
  void asr(UDSPInstruction opc);
  void asr16(UDSPInstruction opc);
  void asrn(UDSPInstruction opc);
  void asrnr(UDSPInstruction opc);
  void asrnrx(UDSPInstruction opc);
  void bloop(UDSPInstruction opc);
  void bloopi(UDSPInstruction opc);
  void call(UDSPInstruction opc);
  void callr(UDSPInstruction opc);
  void clr(UDSPInstruction opc);
  void clrl(UDSPInstruction opc);
  void clrp(UDSPInstruction opc);
  void cmp(UDSPInstruction opc);
  void cmpaxh(UDSPInstruction opc);
  void cmpi(UDSPInstruction opc);
  void cmpis(UDSPInstruction opc);
  void dar(UDSPInstruction opc);
  void dec(UDSPInstruction opc);
  void decm(UDSPInstruction opc);
  void halt(UDSPInstruction opc);
  void iar(UDSPInstruction opc);
  void ifcc(UDSPInstruction opc);
  void ilrr(UDSPInstruction opc);
  void ilrrd(UDSPInstruction opc);
  void ilrri(UDSPInstruction opc);
  void ilrrn(UDSPInstruction opc);
  void inc(UDSPInstruction opc);
  void incm(UDSPInstruction opc);
  void jcc(UDSPInstruction opc);
  void jmprcc(UDSPInstruction opc);
  void loop(UDSPInstruction opc);
  void loopi(UDSPInstruction opc);
  void lr(UDSPInstruction opc);
  void lri(UDSPInstruction opc);
  void lris(UDSPInstruction opc);
  void lrr(UDSPInstruction opc);
  void lrrd(UDSPInstruction opc);
  void lrri(UDSPInstruction opc);
  void lrrn(UDSPInstruction opc);
  void lrs(UDSPInstruction opc);
  void lsl(UDSPInstruction opc);
  void lsl16(UDSPInstruction opc);
  void lsr(UDSPInstruction opc);
  void lsr16(UDSPInstruction opc);
  void lsrn(UDSPInstruction opc);
  void lsrnr(UDSPInstruction opc);
  void lsrnrx(UDSPInstruction opc);
  void madd(UDSPInstruction opc);
  void maddc(UDSPInstruction opc);
  void maddx(UDSPInstruction opc);
  void mov(UDSPInstruction opc);
  void movax(UDSPInstruction opc);
  void movnp(UDSPInstruction opc);
  void movp(UDSPInstruction opc);
  void movpz(UDSPInstruction opc);
  void movr(UDSPInstruction opc);
  void mrr(UDSPInstruction opc);
  void msub(UDSPInstruction opc);
  void msubc(UDSPInstruction opc);
  void msubx(UDSPInstruction opc);
  void mul(UDSPInstruction opc);
  void mulac(UDSPInstruction opc);
  void mulaxh(UDSPInstruction opc);
  void mulc(UDSPInstruction opc);
  void mulcac(UDSPInstruction opc);
  void mulcmv(UDSPInstruction opc);
  void mulcmvz(UDSPInstruction opc);
  void mulmv(UDSPInstruction opc);
  void mulmvz(UDSPInstruction opc);
  void mulx(UDSPInstruction opc);
  void mulxac(UDSPInstruction opc);
  void mulxmv(UDSPInstruction opc);
  void mulxmvz(UDSPInstruction opc);
  void neg(UDSPInstruction opc);
  void nop(UDSPInstruction opc);
  void notc(UDSPInstruction opc);
  void nx(UDSPInstruction opc);
  void orc(UDSPInstruction opc);
  void ori(UDSPInstruction opc);
  void orr(UDSPInstruction opc);
  void ret(UDSPInstruction opc);
  void rti(UDSPInstruction opc);
  void sbclr(UDSPInstruction opc);
  void sbset(UDSPInstruction opc);
  void si(UDSPInstruction opc);
  void sr(UDSPInstruction opc);
  void srbith(UDSPInstruction opc);
  void srr(UDSPInstruction opc);
  void srrd(UDSPInstruction opc);
  void srri(UDSPInstruction opc);
  void srrn(UDSPInstruction opc);
  void srs(UDSPInstruction opc);
  void srsh(UDSPInstruction opc);
  void sub(UDSPInstruction opc);
  void subarn(UDSPInstruction opc);
  void subax(UDSPInstruction opc);
  void subp(UDSPInstruction opc);
  void subr(UDSPInstruction opc);
  void tst(UDSPInstruction opc);
  void tstaxh(UDSPInstruction opc);
  void tstprod(UDSPInstruction opc);
  void xorc(UDSPInstruction opc);
  void xori(UDSPInstruction opc);
  void xorr(UDSPInstruction opc);

  // Extension handlers. They read registers right away but only queue their
  // register writes; ApplyWriteBackLog() commits them after the main handler.
  void l(UDSPInstruction opc);
  void ln(UDSPInstruction opc);
  void ls(UDSPInstruction opc);
  void lsn(UDSPInstruction opc);
  void lsm(UDSPInstruction opc);
  void lsnm(UDSPInstruction opc);
  void sl(UDSPInstruction opc);
  void sln(UDSPInstruction opc);
  void slm(UDSPInstruction opc);
  void slnm(UDSPInstruction opc);
  void s(UDSPInstruction opc);
  void sn(UDSPInstruction opc);
  void ld(UDSPInstruction opc);
  void ldax(UDSPInstruction opc);
  void ldn(UDSPInstruction opc);
  void ldaxn(UDSPInstruction opc);
  void ldm(UDSPInstruction opc);
  void ldaxm(UDSPInstruction opc);
  void ldnm(UDSPInstruction opc);
  void ldaxnm(UDSPInstruction opc);
  void mv(UDSPInstruction opc);
  void dr(UDSPInstruction opc);
  void ir(UDSPInstruction opc);
  void nr(UDSPInstruction opc);
  void nop_ext(UDSPInstruction opc);

private:
  // How an address register moves after an access.
  enum class AddrStep : u8
  {
    Increment,  // +1
    Index,      // + the matching $ix register
  };

  // The post-modify modes of the LRR/SRR/ILRR families.
  enum class PointerStep : u8
  {
    Keep,
    Decrement,
    Increment,
    AddIndex,
  };

  // Which operands of a multiply may be unsigned when SET15 is in effect.
  enum class Signedness : u8
  {
    Signed,
    BothUnsigned,
    FirstUnsigned,
  };

  // What a multiply-and-move instruction puts into its accumulator.
  enum class ProductMove : u8
  {
    AddToAcc,     // acc += old product
    MoveRounded,  // acc = old product rounded to the middle word
    Move,         // acc = old product
  };

  enum class BurstEnd : u8
  {
    Continue,
    Finished,    // out of cycles, halted or idle
    Breakpoint,  // stopped in front of a breakpoint
  };

  struct DeferredWrite
  {
    int reg;
    u16 value;
  };

  SDSP& Regs() { return m_dsp_core.DSPState(); }
  const SDSP& Regs() const { return m_dsp_core.DSPState(); }

  void Execute(UDSPInstruction inst);
  void ServiceLoopEnd();
  BurstEnd RunBurst(int& cycles, int count, bool check_halt, bool check_idle, bool check_breaks);

  bool ConditionHolds(u8 condition) const;

  // Address generation with the $wr modulo.
  u16 AddressPlusOne(int ar) const;
  u16 AddressMinusOne(int ar) const;
  u16 AddressPlus(int ar, s16 step) const;
  u16 AddressMinus(int ar, s16 step) const;
  u16 AddressAfter(int ar, AddrStep step) const;

  // Register views.
  s64 Acc(int which) const;
  void SetAcc(int which, s64 value);
  s32 AxWhole(int which) const;
  s16 AxLow(int which) const;
  s16 AxHigh(int which) const;
  u16 AxHalf(int which, bool high) const;
  s64 Product() const;
  s64 ProductRounded() const;
  void SetProduct(s64 value);

  // Multiplier.
  s64 MultiplyRaw(u16 a, u16 b, Signedness mode) const;
  s64 MultiplyAxHalves(bool ax0_high, bool ax1_high) const;
  void MultiplyAndMove(int acc, ProductMove move, s64 new_product);

  // Flags.
  void SetFlags16(s16 value, bool carry, bool overflow, bool over_s32);
  void SetFlags40(s64 value, bool carry = false, bool overflow = false);
  void SetFlagsAdd(s64 a, s64 b, s64 result);
  void SetFlagsSub(s64 a, s64 b, s64 result);
  void SetLogicZero(bool zero);

  // Shared bodies of instruction groups.
  void StoreAccWithAdd(int acc, s64 a, s64 b);
  void StoreAccWithSub(int acc, s64 a, s64 b);
  void StoreAccMoved(int acc, s64 value);
  void StoreAccShifted(int acc, s64 value);
  void FinishMidLogic(int acc, u16 result);
  void ShiftBy7BitAmount(int acc, u16 amount_field, bool arithmetic, bool positive_is_left);
  s64 SignedAxPart(int selector) const;
  void EnterLoop(u16 last_address, u16 count);
  void LoadThroughPointer(UDSPInstruction opc, PointerStep step);
  void StoreThroughPointer(UDSPInstruction opc, PointerStep step);
  void LoadFromIMEM(UDSPInstruction opc, PointerStep step);
  void UpdatePointer(int ar, PointerStep step);

  // Register operands as instructions see them: $st reads pop, $st writes push,
  // $acX.m reads may saturate, $acX.h writes sign extend.
  u16 OpReadRegister(int reg);
  void OpWriteRegister(int reg, u16 val);
  void ExtendIfMidAccumulator(int reg);
  void LoadRegister(int reg, u16 value);

  // Extension write queue and the extension bodies built on it.
  void WriteToBackLog(int reg, u16 value);
  void ZeroWriteBackLog();
  void ZeroWriteBackLogPreserveAcc(int acc);
  void ExtStore(UDSPInstruction opc, AddrStep step);
  void ExtLoad(UDSPInstruction opc, AddrStep step);
  void ExtLoadStore(UDSPInstruction opc, bool store_through_ar0, AddrStep ar0_step,
                    AddrStep ar3_step);
  void ExtLoadPair(int first_reg, int second_reg, int source_ar, AddrStep source_step,
                   AddrStep ar3_step);
  void LoadAxPair(UDSPInstruction opc, AddrStep source_step, AddrStep ar3_step);
  void LoadAxWhole(UDSPInstruction opc, AddrStep source_step, AddrStep ar3_step);

  DSPCore& m_dsp_core;

  std::array<DeferredWrite, 4> m_deferred{};
  size_t m_deferred_count = 0;
};
}  // namespace DSP::Interpreter
