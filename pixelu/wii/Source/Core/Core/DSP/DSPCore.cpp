// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/DSP/DSPCore.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/MemoryUtil.h"
#include "Common/MsgHandler.h"

#include "Core/DSP/DSPAccelerator.h"
#include "Core/DSP/DSPAnalyzer.h"
#include "Core/DSP/DSPHost.h"
#include "Core/DSP/Interpreter/DSPInterpreter.h"
#include "Core/DSP/Jit/DSPEmitterBase.h"

namespace DSP
{
namespace
{
// Adler-32 checksums of the ROM pairs we know about (boot ROM, coefficient ROM).
struct KnownRomPair
{
  u32 irom_adler;
  u32 drom_adler;
  bool outdated_free_rom;
};

constexpr std::array<KnownRomPair, 7> KNOWN_ROMS{{
    {0x66f334fe, 0xf3b93527, false},  // dumped from a console
    {0x9c8f593c, 0x10000001, true},   // free ROM 0.1
    {0xd9907f71, 0xb019c2fb, true},   // free ROM 0.2
    {0xd9907f71, 0xdb6880c1, true},   // free ROM 0.2.1
    {0x3aa4a793, 0xa4a575f5, true},   // free ROM 0.3
    {0x128ea7a2, 0xa4a575f5, true},   // free ROM 0.3.1
    {0xe789b5a5, 0xa4a575f5, false},  // free ROM 0.4 (current)
}};

// Warns about unknown or outdated ROMs. Returns false when the user chose to stop.
bool AcceptRoms(const SDSP& dsp)
{
  const u32 irom_adler =
      Common::HashAdler32(reinterpret_cast<const u8*>(dsp.irom), DSP_IROM_BYTE_SIZE);
  const u32 drom_adler =
      Common::HashAdler32(reinterpret_cast<const u8*>(dsp.coef), DSP_COEF_BYTE_SIZE);

  const auto match = std::ranges::find_if(KNOWN_ROMS, [&](const KnownRomPair& rom) {
    return rom.irom_adler == irom_adler && rom.drom_adler == drom_adler;
  });

  if (match == KNOWN_ROMS.end())
  {
    const bool stop = AskYesNoFmtT(
        "Your DSP ROMs have incorrect hashes.\n\n"
        "Delete the dsp_rom.bin and dsp_coef.bin files in the GC folder in the User "
        "folder to use the free DSP ROM, or replace them with good dumps from a real "
        "GameCube/Wii.\n\n"
        "Would you like to stop now to fix the problem?\n"
        "If you select \"No\", audio might be garbled.");
    return !stop;
  }

  if (match->outdated_free_rom)
  {
    const bool stop = AskYesNoFmtT(
        "You are using an old free DSP ROM made by the Dolphin Team.\n"
        "Due to emulation accuracy improvements, this ROM no longer works correctly.\n\n"
        "Delete the dsp_rom.bin and dsp_coef.bin files in the GC folder in the User folder "
        "to use the most recent free DSP ROM, or replace them with good dumps from a real "
        "GameCube/Wii.\n\n"
        "Would you like to stop now to fix the problem?\n"
        "If you select \"No\", audio might be garbled.");
    return !stop;
  }

  return true;
}

// The accelerator as wired into the LLE DSP: it reads ARAM through the host and
// reports its end-of-buffer conditions as DSP exceptions.
class ChipAccelerator final : public Accelerator
{
public:
  explicit ChipAccelerator(SDSP& dsp) : m_owner(dsp) {}

protected:
  u8 ReadMemory(u32 address) override { return Host::ReadHostMemory(address); }
  void WriteMemory(u32 address, u8 value) override { Host::WriteHostMemory(value, address); }

  void OnRawReadEndException() override
  {
    m_owner.SetException(ExceptionType::AcceleratorRawReadOverflow);
  }
  void OnRawWriteEndException() override
  {
    m_owner.SetException(ExceptionType::AcceleratorRawWriteOverflow);
  }
  void OnSampleReadEndException() override
  {
    m_owner.SetException(ExceptionType::AcceleratorSampleReadOverflow);
  }

private:
  SDSP& m_owner;
};

u16* AllocateWords(u32 byte_size)
{
  return static_cast<u16*>(Common::AllocateMemoryPages(byte_size));
}

void ReleaseWords(u16*& words, u32 byte_size)
{
  Common::FreeMemoryPages(words, byte_size);
  words = nullptr;
}
}  // namespace

SDSP::SDSP(DSPCore& core) : m_dsp_core{core}
{
}

SDSP::~SDSP() = default;

bool SDSP::Initialize(const DSPInitOptions& opts)
{
  m_step_counter = 0;
  m_accelerator = std::make_unique<ChipAccelerator>(*this);

  irom = AllocateWords(DSP_IROM_BYTE_SIZE);
  iram = AllocateWords(DSP_IRAM_BYTE_SIZE);
  dram = AllocateWords(DSP_DRAM_BYTE_SIZE);
  coef = AllocateWords(DSP_COEF_BYTE_SIZE);

  std::copy_n(opts.irom_contents.data(), DSP_IROM_SIZE, irom);
  std::copy_n(opts.coef_contents.data(), DSP_COEF_SIZE, coef);

  if (!AcceptRoms(*this))
  {
    ReleaseMemories();
    return false;
  }

  // Power-on register state. memset also clears the padding, which savestates store.
  std::memset(&r, 0, sizeof(r));
  std::fill(std::begin(reg_stack_ptrs), std::end(reg_stack_ptrs), u8{0});
  for (auto& stack : reg_stacks)
    std::fill(std::begin(stack), std::end(stack), u16{0});
  std::fill(std::begin(r.wr), std::end(r.wr), u16{0xffff});
  r.sr |= SR_INT_ENABLE | SR_EXT_INT_ENABLE;

  // IRAM starts out full of HALT instructions, DRAM is cleared.
  std::fill_n(iram, DSP_IRAM_SIZE, u16{0x0021});
  std::fill_n(dram, DSP_DRAM_SIZE, u16{0});

  control_reg = CR_INIT | CR_HALT;
  InitializeIFX();

  // IRAM is only written by DMA and savestates, which lift the protection briefly.
  Common::WriteProtectMemory(iram, DSP_IRAM_BYTE_SIZE, false);
  return true;
}

void SDSP::Reset()
{
  pc = DSP_RESET_VECTOR;
  std::fill(std::begin(r.wr), std::end(r.wr), u16{0xffff});
}

void SDSP::Shutdown()
{
  ReleaseMemories();
}

void SDSP::ReleaseMemories()
{
  ReleaseWords(irom, DSP_IROM_BYTE_SIZE);
  ReleaseWords(iram, DSP_IRAM_BYTE_SIZE);
  ReleaseWords(dram, DSP_DRAM_BYTE_SIZE);
  ReleaseWords(coef, DSP_COEF_BYTE_SIZE);
}

void SDSP::SetException(ExceptionType exception)
{
  exceptions |= static_cast<u8>(1u << static_cast<int>(exception));
}

void SDSP::SetExternalInterrupt(bool val)
{
  external_interrupt_waiting.store(val, std::memory_order_release);
}

void SDSP::CheckExternalInterrupt()
{
  if (!IsSRFlagSet(SR_EXT_INT_ENABLE))
    return;

  SetException(ExceptionType::ExternalInterrupt);
  control_reg &= static_cast<u16>(~CR_EXTERNAL_INT);
}

bool SDSP::CheckExceptions()
{
  if (exceptions == 0)
    return false;

  constexpr int external_level = static_cast<int>(ExceptionType::ExternalInterrupt);

  // Higher levels win. The mail interrupt has its own enable bit; the others share one.
  for (int level = external_level; level >= 1; --level)
  {
    const u8 bit = static_cast<u8>(1u << level);
    if ((exceptions & bit) == 0)
      continue;

    const bool is_external = level == external_level;
    if (!is_external && !IsSRFlagSet(SR_INT_ENABLE))
    {
#if defined(_DEBUG) || defined(DEBUGFAST)
      ERROR_LOG_FMT(DSPLLE, "Exception {} is pending but interrupts are disabled", level);
#endif
      continue;
    }

    // The handler returns with RTI, which pops both of these.
    StoreStack(StackRegister::Call, pc);
    StoreStack(StackRegister::Data, r.sr);

    pc = static_cast<u16>(level * 2);
    exceptions &= static_cast<u8>(~bit);
    r.sr &= static_cast<u16>(is_external ? ~SR_EXT_INT_ENABLE : ~SR_INT_ENABLE);
    return true;
  }

  return false;
}

u16 SDSP::ReadRegister(size_t reg) const
{
  if (reg < 0x10)
  {
    // $0-$15 are four banks of four 16-bit registers.
    const size_t slot = reg & 3;
    switch (reg >> 2)
    {
    case 0:
      return r.ar[slot];
    case 1:
      return r.ix[slot];
    case 2:
      return r.wr[slot];
    default:
      return r.st[slot];
    }
  }

  switch (reg)
  {
  case DSP_REG_ACH0:
  case DSP_REG_ACH1:
    return static_cast<u16>(r.ac[reg - DSP_REG_ACH0].h);
  case DSP_REG_CR:
    return r.cr;
  case DSP_REG_SR:
    return r.sr;
  case DSP_REG_PRODL:
    return r.prod.l;
  case DSP_REG_PRODM:
    return r.prod.m;
  case DSP_REG_PRODH:
    return r.prod.h;
  case DSP_REG_PRODM2:
    return r.prod.m2;
  case DSP_REG_AXL0:
  case DSP_REG_AXL1:
    return r.ax[reg - DSP_REG_AXL0].l;
  case DSP_REG_AXH0:
  case DSP_REG_AXH1:
    return r.ax[reg - DSP_REG_AXH0].h;
  case DSP_REG_ACL0:
  case DSP_REG_ACL1:
    return r.ac[reg - DSP_REG_ACL0].l;
  case DSP_REG_ACM0:
  case DSP_REG_ACM1:
    return r.ac[reg - DSP_REG_ACM0].m;
  default:
    ASSERT_MSG(DSPLLE, false, "Read of nonexistent DSP register {}", reg);
    return 0;
  }
}

void SDSP::WriteRegister(size_t reg, u16 val)
{
  if (reg < 0x10)
  {
    const size_t slot = reg & 3;
    switch (reg >> 2)
    {
    case 0:
      r.ar[slot] = val;
      break;
    case 1:
      r.ix[slot] = val;
      break;
    case 2:
      r.wr[slot] = val;
      break;
    default:
      r.st[slot] = val;
      break;
    }
    return;
  }

  switch (reg)
  {
  case DSP_REG_ACH0:
  case DSP_REG_ACH1:
    r.ac[reg - DSP_REG_ACH0].h = val;
    break;
  case DSP_REG_CR:
    r.cr = val;
    break;
  case DSP_REG_SR:
    r.sr = val;
    break;
  case DSP_REG_PRODL:
    r.prod.l = val;
    break;
  case DSP_REG_PRODM:
    r.prod.m = val;
    break;
  case DSP_REG_PRODH:
    r.prod.h = val;
    break;
  case DSP_REG_PRODM2:
    r.prod.m2 = val;
    break;
  case DSP_REG_AXL0:
  case DSP_REG_AXL1:
    r.ax[reg - DSP_REG_AXL0].l = val;
    break;
  case DSP_REG_AXH0:
  case DSP_REG_AXH1:
    r.ax[reg - DSP_REG_AXH0].h = val;
    break;
  case DSP_REG_ACL0:
  case DSP_REG_ACL1:
    r.ac[reg - DSP_REG_ACL0].l = val;
    break;
  case DSP_REG_ACM0:
  case DSP_REG_ACM1:
    r.ac[reg - DSP_REG_ACM0].m = val;
    break;
  default:
    break;
  }
}

void SDSP::DoState(PointerWrap& p)
{
  // Field order is the savestate format.
  p.Do(r);
  p.Do(pc);
  p.Do(control_reg);
  p.Do(control_reg_init_code_clear_time);
  p.Do(reg_stack_ptrs);
  p.Do(exceptions);
  p.Do(external_interrupt_waiting);
  for (auto& stack : reg_stacks)
    p.Do(stack);
  p.Do(m_step_counter);
  p.DoArray(m_ifx_regs);
  m_accelerator->DoState(p);
  p.Do(m_mailbox[0]);
  p.Do(m_mailbox[1]);

  Common::UnWriteProtectMemory(iram, DSP_IRAM_BYTE_SIZE, false);
  p.DoArray(iram, DSP_IRAM_SIZE);
  Common::WriteProtectMemory(iram, DSP_IRAM_BYTE_SIZE, false);

  // A loaded state may carry different code. This passes the words in host order and
  // the byte size of IRAM, so the CRC differs from the one computed at DMA time; the
  // dumps and hashes made from here have always worked this way.
  if (p.IsReadMode())
    Host::CodeLoaded(m_dsp_core, reinterpret_cast<const u8*>(iram), DSP_IRAM_BYTE_SIZE);

  p.DoArray(dram, DSP_DRAM_SIZE);
}

DSPCore::DSPCore()
    : m_dsp{*this}, m_dsp_interpreter{std::make_unique<Interpreter::Interpreter>(*this)}
{
}

DSPCore::~DSPCore() = default;

bool DSPCore::Initialize(const DSPInitOptions& opts)
{
  if (!m_dsp.Initialize(opts))
    return false;

  if (opts.core_type == DSPInitOptions::CoreType::JIT64)
    m_dsp_jit = JIT::CreateDSPEmitter(*this);

  m_dsp_cap.reset(opts.capture_logger);

  m_core_state = State::Running;
  return true;
}

void DSPCore::Shutdown()
{
  if (m_core_state == State::Stopped)
    return;

  m_core_state = State::Stopped;
  m_dsp_jit.reset();
  m_dsp.Shutdown();
  m_dsp_cap.reset();
}

int DSPCore::RunCycles(int cycles)
{
  if (m_dsp_jit)
    return m_dsp_jit->RunCycles(static_cast<u16>(cycles));

  while (cycles > 0)
  {
    if (m_core_state == State::Running)
    {
#if defined(_DEBUG) || defined(DEBUGFAST)
      cycles = m_dsp_interpreter->RunCyclesDebug(cycles);
#else
      cycles = m_dsp_interpreter->RunCycles(cycles);
#endif
    }
    else if (m_core_state == State::Stepping)
    {
      // Wait for the debugger to allow one instruction.
      m_step_event.Wait();
      if (m_core_state == State::Stepping)
      {
        m_dsp_interpreter->Step();
        --cycles;
      }
    }
    // A stopped core spins until another thread changes the state.
  }

  return cycles;
}

void DSPCore::Step()
{
  if (m_core_state == State::Stepping)
    m_step_event.Set();
}

void DSPCore::Reset()
{
  m_dsp.Reset();
  m_dsp.GetAnalyzer().Analyze(m_dsp);
}

void DSPCore::ClearIRAM()
{
  if (m_dsp_jit)
    m_dsp_jit->ClearIRAM();
}

void DSPCore::SetState(State new_state)
{
  m_core_state = new_state;

  // Wake RunCycles if it is waiting for a step.
  if (new_state == State::Running)
    m_step_event.Set();
}

State DSPCore::GetState() const
{
  return m_core_state;
}

void DSPCore::SetException(ExceptionType exception)
{
  m_dsp.SetException(exception);
}

void DSPCore::SetExternalInterrupt(bool val)
{
  m_dsp.SetExternalInterrupt(val);
}

void DSPCore::CheckExternalInterrupt()
{
  m_dsp.CheckExternalInterrupt();
}

bool DSPCore::CheckExceptions()
{
  return m_dsp.CheckExceptions();
}

u16 DSPCore::ReadRegister(size_t reg) const
{
  return m_dsp.ReadRegister(reg);
}

void DSPCore::WriteRegister(size_t reg, u16 val)
{
  m_dsp.WriteRegister(reg, val);
}

u32 DSPCore::PeekMailbox(Mailbox mailbox) const
{
  return m_dsp.PeekMailbox(mailbox);
}

u16 DSPCore::ReadMailboxLow(Mailbox mailbox)
{
  return m_dsp.ReadMailboxLow(mailbox);
}

u16 DSPCore::ReadMailboxHigh(Mailbox mailbox)
{
  return m_dsp.ReadMailboxHigh(mailbox);
}

void DSPCore::WriteMailboxLow(Mailbox mailbox, u16 value)
{
  m_dsp.WriteMailboxLow(mailbox, value);
}

void DSPCore::WriteMailboxHigh(Mailbox mailbox, u16 value)
{
  m_dsp.WriteMailboxHigh(mailbox, value);
}

void DSPCore::LogIFXRead(u16 address, u16 read_value)
{
  m_dsp_cap->LogIFXRead(address, read_value);
}

void DSPCore::LogIFXWrite(u16 address, u16 written_value)
{
  m_dsp_cap->LogIFXWrite(address, written_value);
}

void DSPCore::LogDMA(u16 control, u32 gc_address, u16 dsp_address, u16 length, const u8* data)
{
  m_dsp_cap->LogDMA(control, gc_address, dsp_address, length, data);
}

bool DSPCore::IsJITCreated() const
{
  return m_dsp_jit != nullptr;
}

void DSPCore::DoState(PointerWrap& p)
{
  m_dsp.DoState(p);
  if (m_dsp_jit)
    m_dsp_jit->DoState(p);
}
}  // namespace DSP
