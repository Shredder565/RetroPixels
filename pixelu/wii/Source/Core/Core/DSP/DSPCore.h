// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// State and top-level driver of the low-level emulated audio DSP (the 16-bit
// Macronix DSP in the GameCube "Flipper" and the Wii "Hollywood" chips).
//
// Memory map seen by the DSP (all word addressed):
//   instruction space: 0x0000-0x0fff IRAM, 0x8000-0x8fff IROM
//   data space:        0x0000-0x0fff DRAM, 0x1000-0x17ff COEF (DROM),
//                      0xff00-0xffff hardware registers ("IFX")

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

#include "Common/Event.h"
#include "Core/DSP/DSPAnalyzer.h"
#include "Core/DSP/DSPBreakpoints.h"
#include "Core/DSP/DSPCaptureLogger.h"

class PointerWrap;

namespace DSP
{
class Accelerator;
class DSPCore;

namespace Interpreter
{
class Interpreter;
}

namespace JIT
{
class DSPEmitter;
}

// Sizes of the four memories, in bytes and in 16-bit words.
enum : u32
{
  DSP_IRAM_BYTE_SIZE = 0x2000,
  DSP_IRAM_SIZE = 0x1000,
  DSP_IRAM_MASK = 0x0fff
};

enum : u32
{
  DSP_IROM_BYTE_SIZE = 0x2000,
  DSP_IROM_SIZE = 0x1000,
  DSP_IROM_MASK = 0x0fff
};

enum : u32
{
  DSP_DRAM_BYTE_SIZE = 0x2000,
  DSP_DRAM_SIZE = 0x1000,
  DSP_DRAM_MASK = 0x0fff
};

enum : u32
{
  DSP_COEF_BYTE_SIZE = 0x1000,
  DSP_COEF_SIZE = 0x800,
  DSP_COEF_MASK = 0x7ff
};

// Where execution starts after a reset (the start of IROM).
enum : u16
{
  DSP_RESET_VECTOR = 0x8000
};

// Depth of each of the four hardware stacks (we allow more than the real chip).
enum : u8
{
  DSP_STACK_DEPTH = 0x20,
  DSP_STACK_MASK = 0x1f
};

// Bits of the DSP DMA control register (DSCR).
enum : u32
{
  DSP_CR_IMEM = 2,
  DSP_CR_DMEM = 0,
  DSP_CR_TO_CPU = 1,
  DSP_CR_FROM_CPU = 0
};

// Register numbers, as used in instruction encodings ($0 .. $31).
enum : int
{
  DSP_REG_AR0 = 0x00,  // address registers
  DSP_REG_AR1 = 0x01,
  DSP_REG_AR2 = 0x02,
  DSP_REG_AR3 = 0x03,

  DSP_REG_IX0 = 0x04,  // index (increment) registers
  DSP_REG_IX1 = 0x05,
  DSP_REG_IX2 = 0x06,
  DSP_REG_IX3 = 0x07,

  DSP_REG_WR0 = 0x08,  // wrap (modulo) registers
  DSP_REG_WR1 = 0x09,
  DSP_REG_WR2 = 0x0a,
  DSP_REG_WR3 = 0x0b,

  DSP_REG_ST0 = 0x0c,  // call, data, loop address and loop counter stacks
  DSP_REG_ST1 = 0x0d,
  DSP_REG_ST2 = 0x0e,
  DSP_REG_ST3 = 0x0f,

  DSP_REG_ACH0 = 0x10,  // accumulator high bytes (8 bits, sign extended)
  DSP_REG_ACH1 = 0x11,

  DSP_REG_CR = 0x12,  // page register for the short-address loads and stores
  DSP_REG_SR = 0x13,  // status register

  DSP_REG_PRODL = 0x14,  // product register, kept as four parts
  DSP_REG_PRODM = 0x15,
  DSP_REG_PRODH = 0x16,
  DSP_REG_PRODM2 = 0x17,

  DSP_REG_AXL0 = 0x18,  // the two 32-bit "AX" registers
  DSP_REG_AXL1 = 0x19,
  DSP_REG_AXH0 = 0x1a,
  DSP_REG_AXH1 = 0x1b,

  DSP_REG_ACL0 = 0x1c,  // accumulator low and middle words
  DSP_REG_ACL1 = 0x1d,
  DSP_REG_ACM0 = 0x1e,
  DSP_REG_ACM1 = 0x1f,
};

// Pseudo register numbers naming whole registers; used by the opcode tables only.
enum : int
{
  DSP_REG_ACC0_FULL = 0x20,
  DSP_REG_ACC1_FULL = 0x21,
  DSP_REG_AX0_FULL = 0x22,
  DSP_REG_AX1_FULL = 0x23,
};

// Hardware register addresses (low byte of 0xffxx).
enum : u32
{
  DSP_COEF_A1_0 = 0xa0,  // first of the 16 ADPCM coefficient registers

  DSP_DSCR = 0xc9,   // DMA control
  DSP_DSPA = 0xcd,   // DMA address on the DSP side
  DSP_DSBL = 0xcb,   // DMA length (writing it starts the transfer)
  DSP_DSMAH = 0xce,  // DMA main memory address, high and low halves
  DSP_DSMAL = 0xcf,

  DSP_FORMAT = 0xd1,  // accelerator sample format
  DSP_ACUNK = 0xd2,
  DSP_ACDRAW = 0xd3,  // accelerator raw data
  DSP_ACSAH = 0xd4,   // accelerator start (loop) address
  DSP_ACSAL = 0xd5,
  DSP_ACEAH = 0xd6,  // accelerator end address
  DSP_ACEAL = 0xd7,
  DSP_ACCAH = 0xd8,  // accelerator current address
  DSP_ACCAL = 0xd9,
  DSP_PRED_SCALE = 0xda,  // ADPCM predictor index and scale
  DSP_YN1 = 0xdb,         // ADPCM history
  DSP_YN2 = 0xdc,
  DSP_ACDSAMP = 0xdd,  // decoded sample read
  DSP_GAIN = 0xde,
  DSP_ACIN = 0xdf,  // PCM input for the MMIO sample modes

  DSP_AMDM = 0xef,  // ARAM DMA request mask

  DSP_DIRQ = 0xfb,  // interrupt request to the CPU
  DSP_DMBH = 0xfc,  // DSP -> CPU mailbox
  DSP_DMBL = 0xfd,
  DSP_CMBH = 0xfe,  // CPU -> DSP mailbox
  DSP_CMBL = 0xff
};

enum class StackRegister
{
  Call,
  Data,
  LoopAddress,
  LoopCounter
};

// Bits of the control register shared with the CPU side (HW/DSP.cpp), not $cr.
enum : u32
{
  CR_RESET = 0x0001,
  CR_EXTERNAL_INT = 0x0002,
  CR_HALT = 0x0004,
  CR_INIT_CODE = 0x0400,
  CR_INIT = 0x0800
};

// Status register ($sr) bits.
enum : u16
{
  SR_CARRY = 0x0001,
  SR_OVERFLOW = 0x0002,
  SR_ARITH_ZERO = 0x0004,
  SR_SIGN = 0x0008,
  SR_OVER_S32 = 0x0010,         // result does not fit in 32 bits
  SR_TOP2BITS = 0x0020,         // bits 31 and 30 of the result are equal
  SR_LOGIC_ZERO = 0x0040,       // set by ANDF/ANDCF
  SR_OVERFLOW_STICKY = 0x0080,  // like SR_OVERFLOW but only cleared by software
  SR_100 = 0x0100,              // always reads back as zero
  SR_INT_ENABLE = 0x0200,
  SR_400 = 0x0400,
  SR_EXT_INT_ENABLE = 0x0800,
  SR_1000 = 0x1000,
  SR_MUL_MODIFY = 0x2000,    // clear: products are doubled (M2), set: not (M0)
  SR_40_MODE_BIT = 0x4000,   // SET40: sign extension and saturation of $acX.m
  SR_MUL_UNSIGNED = 0x8000,  // SET15: MULX-style products treat $axX.l as unsigned

  // The arithmetic flags rewritten by every compare/arithmetic result.
  SR_CMP_MASK = 0x3f
};

// Interrupt/exception levels. Level n vectors to address 2 * n.
enum class ExceptionType
{
  StackOverflow = 1,
  EXP_2 = 2,
  AcceleratorRawReadOverflow = 3,
  AcceleratorRawWriteOverflow = 4,
  AcceleratorSampleReadOverflow = 5,
  EXP_6 = 6,
  ExternalInterrupt = 7
};

enum class Mailbox
{
  CPU,
  DSP
};

// The register file. Savestates store this struct as raw bytes, so its layout is fixed.
struct DSP_Regs
{
  u16 ar[4];
  u16 ix[4];
  u16 wr[4];
  u16 st[4];
  u16 cr;
  u16 sr;

  union
  {
    u64 val;
    struct
    {
      u16 l;
      u16 m;
      u16 h;
      u16 m2;
    };
  } prod;

  union
  {
    u32 val;
    struct
    {
      u16 l;
      u16 h;
    };
  } ax[2];

  // h is 32 bits wide so that val holds the 40-bit accumulator sign extended to 64 bits.
  union
  {
    u64 val;
    struct
    {
      u16 l;
      u16 m;
      u32 h;
    };
  } ac[2];
};

struct DSPInitOptions
{
  // Contents of the boot ROM (IROM) and of the coefficient ROM (DROM).
  std::array<u16, DSP_IROM_SIZE> irom_contents{};
  std::array<u16, DSP_COEF_SIZE> coef_contents{};

  enum class CoreType
  {
    Interpreter,
    JIT64,
  };
  CoreType core_type = CoreType::JIT64;

  // Receives every IFX access and DMA. DSPCore takes ownership when it initializes.
  DSPCaptureLogger* capture_logger;

  DSPInitOptions() : capture_logger(new DefaultDSPCaptureLogger()) {}
};

// Everything the DSP itself can see: registers, memories, stacks, hardware registers.
struct SDSP
{
  explicit SDSP(DSPCore& core);
  ~SDSP();

  SDSP(const SDSP&) = delete;
  SDSP& operator=(const SDSP&) = delete;

  SDSP(SDSP&&) = delete;
  SDSP& operator=(SDSP&&) = delete;

  // Allocates the memories and puts the chip in its power-on state.
  bool Initialize(const DSPInitOptions& opts);
  void Shutdown();

  // What the reset vector does: jump to the boot ROM.
  void Reset();

  void InitializeIFX();
  void WriteIFX(u32 address, u16 value);
  u16 ReadIFX(u16 address);

  // Mailboxes. Bit 31 of the 32-bit value is the "new mail" flag.
  u32 PeekMailbox(Mailbox mailbox) const;
  u16 ReadMailboxLow(Mailbox mailbox);
  u16 ReadMailboxHigh(Mailbox mailbox);
  void WriteMailboxLow(Mailbox mailbox, u16 value);
  void WriteMailboxHigh(Mailbox mailbox, u16 value);

  u16 ReadIMEM(u16 address) const;
  u16 ReadDMEM(u16 address);
  void WriteDMEM(u16 address, u16 value);

  // Instruction stream access at pc.
  u16 FetchInstruction();
  u16 PeekInstruction() const;
  void SkipInstruction();

  void SetSRFlag(u16 flag) { r.sr |= flag; }
  bool IsSRFlagSet(u16 flag) const { return (r.sr & flag) != 0; }

  // Marks an exception as pending; CheckExceptions() takes the highest enabled one.
  void SetException(ExceptionType exception);
  bool CheckExceptions();

  // Mail interrupt from the CPU: deferred (thread mode) or raised right away.
  void SetExternalInterrupt(bool val);
  void CheckExternalInterrupt();

  void StoreStack(StackRegister stack_reg, u16 val);
  u16 PopStack(StackRegister stack_reg);

  // Plain register file access (no stack or saturation side effects).
  u16 ReadRegister(size_t reg) const;
  void WriteRegister(size_t reg, u16 val);

  void AdvanceStepCounter() { ++m_step_counter; }
  void SetIRAMCRC(u32 crc) { m_iram_crc = crc; }

  void DoState(PointerWrap& p);

  Analyzer& GetAnalyzer() { return m_analyzer; }
  const Analyzer& GetAnalyzer() const { return m_analyzer; }

  DSP_Regs r{};
  u16 pc = 0;

  // The control register shared with the CPU (CR_* bits); see HW/DSP.cpp.
  u16 control_reg = 0;
  u64 control_reg_init_code_clear_time = 0;

  u8 reg_stack_ptrs[4]{};
  u8 exceptions = 0;  // one pending bit per ExceptionType
  std::atomic<bool> external_interrupt_waiting = false;
  bool reset_dspjit_codespace = false;

  // Stack storage. $stN holds the top entry; pushing moves the old top in here.
  u16 reg_stacks[4][DSP_STACK_DEPTH]{};

  // The memories. The JIT bakes these addresses into its code, so they never move.
  u16* iram = nullptr;
  u16* dram = nullptr;
  u16* irom = nullptr;
  u16* coef = nullptr;

private:
  std::atomic<u32>& MailboxSlot(Mailbox mailbox);
  const std::atomic<u32>& MailboxSlot(Mailbox mailbox) const;

  void ReleaseMemories();

  void RunDMA();
  const u8* CopyToDRAM(u16 byte_offset, u32 addr, u32 size);
  const u8* CopyFromDRAM(u16 byte_offset, u32 addr, u32 size);
  const u8* CopyToIRAM(u16 byte_offset, u32 addr, u32 size);
  const u8* CopyFromIRAM(u16 byte_offset, u32 addr, u32 size);

  u16 ReadIFXRegister(u16 address);

  u32 m_iram_crc = 0;
  u64 m_step_counter = 0;

  // Backing store for the hardware registers that are plain storage.
  std::array<u16, 256> m_ifx_regs{};

  std::unique_ptr<Accelerator> m_accelerator;
  std::array<std::atomic<u32>, 2> m_mailbox;
  DSPCore& m_dsp_core;
  Analyzer m_analyzer;
};

enum class State
{
  Stopped,
  Running,
  Stepping,
};

class DSPCore
{
public:
  DSPCore();
  ~DSPCore();

  DSPCore(const DSPCore&) = delete;
  DSPCore& operator=(const DSPCore&) = delete;

  DSPCore(DSPCore&&) = delete;
  DSPCore& operator=(DSPCore&&) = delete;

  // Takes ownership of the capture logger in opts.
  bool Initialize(const DSPInitOptions& opts);
  void Shutdown();

  // Runs up to the given number of cycles on the JIT or the interpreter; returns the rest.
  int RunCycles(int cycles);

  // Lets one instruction run while in the Stepping state.
  void Step();

  void Reset();
  void ClearIRAM();

  void SetState(State new_state);
  State GetState() const;

  // These forward to the DSP state (SDSP).
  void SetException(ExceptionType exception) { m_dsp.SetException(exception); }
  void SetExternalInterrupt(bool val) { m_dsp.SetExternalInterrupt(val); }
  void CheckExternalInterrupt() { m_dsp.CheckExternalInterrupt(); }
  // Out of line: the JIT calls it from generated code.
  bool CheckExceptions();

  u16 ReadRegister(size_t reg) const { return m_dsp.ReadRegister(reg); }
  void WriteRegister(size_t reg, u16 val) { m_dsp.WriteRegister(reg, val); }

  u32 PeekMailbox(Mailbox mailbox) const { return m_dsp.PeekMailbox(mailbox); }
  u16 ReadMailboxLow(Mailbox mailbox) { return m_dsp.ReadMailboxLow(mailbox); }
  u16 ReadMailboxHigh(Mailbox mailbox) { return m_dsp.ReadMailboxHigh(mailbox); }
  void WriteMailboxLow(Mailbox mailbox, u16 value) { m_dsp.WriteMailboxLow(mailbox, value); }
  void WriteMailboxHigh(Mailbox mailbox, u16 value) { m_dsp.WriteMailboxHigh(mailbox, value); }

  // Forward to the capture logger.
  void LogIFXRead(u16 address, u16 read_value) { m_dsp_cap->LogIFXRead(address, read_value); }
  void LogIFXWrite(u16 address, u16 written_value)
  {
    m_dsp_cap->LogIFXWrite(address, written_value);
  }
  void LogDMA(u16 control, u32 gc_address, u16 dsp_address, u16 length, const u8* data)
  {
    m_dsp_cap->LogDMA(control, gc_address, dsp_address, length, data);
  }

  bool IsJITCreated() const;

  void DoState(PointerWrap& p);

  DSPBreakpoints& BreakPoints() { return m_dsp_breakpoints; }
  const DSPBreakpoints& BreakPoints() const { return m_dsp_breakpoints; }

  SDSP& DSPState() { return m_dsp; }
  const SDSP& DSPState() const { return m_dsp; }

  Interpreter::Interpreter& GetInterpreter() { return *m_dsp_interpreter; }
  const Interpreter::Interpreter& GetInterpreter() const { return *m_dsp_interpreter; }

private:
  // The interpreter's side of RunCycles(); returns the cycles left over.
  int RunInterpreter(int cycles);

  SDSP m_dsp;
  DSPBreakpoints m_dsp_breakpoints;
  State m_core_state = State::Stopped;
  std::unique_ptr<Interpreter::Interpreter> m_dsp_interpreter;
  std::unique_ptr<JIT::DSPEmitter> m_dsp_jit;
  std::unique_ptr<DSPCaptureLogger> m_dsp_cap;
  Common::Event m_step_event;
};
}  // namespace DSP
