// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Audio DMA: the AI pulls 16-bit stereo samples from RAM through a small FIFO
// in 32-byte blocks. At 32 kHz that is one block every 1/4000 s, and
// UpdateAudioDMA() is called at that rate. When a transfer (re)starts, the
// address and length registers are latched and the AID interrupt is raised,
// so software can program the next buffer while the current one plays.

#include "Core/HW/DSP.h"

#include <cstring>
#include <memory>

#include "AudioCommon/AudioCommon.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/MemoryUtil.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"

#include "Core/CoreTiming.h"
#include "Core/DSPEmulator.h"
#include "Core/HW/HSP/HSP.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace DSP
{
namespace
{
// Register offsets.
enum : u32
{
  DSP_MAIL_TO_DSP_HI = 0x5000,
  DSP_MAIL_TO_DSP_LO = 0x5002,
  DSP_MAIL_FROM_DSP_HI = 0x5004,
  DSP_MAIL_FROM_DSP_LO = 0x5006,
  DSP_CONTROL = 0x500A,
  DSP_INTERRUPT_CONTROL = 0x5010,
  AR_INFO = 0x5012,
  AR_MODE = 0x5016,
  AR_REFRESH = 0x501a,
  AR_DMA_MMADDR_H = 0x5020,
  AR_DMA_MMADDR_L = 0x5022,
  AR_DMA_ARADDR_H = 0x5024,
  AR_DMA_ARADDR_L = 0x5026,
  AR_DMA_CNT_H = 0x5028,
  AR_DMA_CNT_L = 0x502A,
  AUDIO_DMA_START_HI = 0x5030,
  AUDIO_DMA_START_LO = 0x5032,
  AUDIO_DMA_BLOCKS_LENGTH = 0x5034,
  AUDIO_DMA_CONTROL_LEN = 0x5036,
  AUDIO_DMA_BLOCKS_LEFT = 0x503A,
};

constexpr u16 ALL_INTERRUPTS = INT_DSP | INT_ARAM | INT_AID;

// Control register: bits taken from a CPU write (everything except the
// interrupt status bits and the DMA busy flag).
constexpr u16 CONTROL_WRITABLE = 0xFD57;
constexpr u16 CONTROL_RESET = 0x0001;
constexpr u16 CONTROL_HALT = 0x0004;
constexpr u16 CONTROL_DMA_BUSY = 0x0200;
constexpr u16 CONTROL_PADDING = 0xF000;

// Writable bits of the plain registers.
constexpr u16 MASK_AR_INFO = 0x007f;
constexpr u16 MASK_AR_REFRESH = 0x07ff;
constexpr u16 MASK_ADDRESS_HIGH = 0x03ff;
constexpr u16 MASK_ADDRESS_LOW = 0xffe0;  // 32-byte aligned
constexpr u16 MASK_COUNT_DIRECTION = 0x8000;
constexpr u16 MASK_AUDIO_HIGH_GAMECUBE = 0x03ff;
constexpr u16 MASK_AUDIO_HIGH_WII = 0x1fff;

constexpr u16 AUDIO_DMA_ENABLE = 0x8000;
constexpr u16 AUDIO_DMA_BLOCKS = 0x7FFF;

constexpr u32 ARAM_COUNT_BITS = 0x7FFFFFFF;
constexpr u32 ARAM_TO_MAIN = 0x80000000;

// LLE: DSP cycles run on every read of a mailbox's high half.
constexpr int DSP_MAIL_SLICE = 72;

// Cycles per 32 bytes of ARAM DMA (measured on hardware).
constexpr int ARAM_DMA_CYCLES_PER_BLOCK = 246;

// Delay of the AID interrupt after audio DMA is enabled. Several Namco games
// crash at boot when it comes sooner than about 87 cycles.
constexpr s64 AUDIO_DMA_START_DELAY = 200;

// Both DMA directions wrap at 64 MiB on the ARAM and main memory side.
constexpr u32 DMA_ADDRESS_WRAP = 0x3ffffff;

u16* High(u32* word)
{
  return MMIO::Utils::HighPart(word);
}
u16* Low(u32* word)
{
  return MMIO::Utils::LowPart(word);
}

u64 LoadU64(const u8* p)
{
  u64 v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
void StoreU64(u8* p, u64 v)
{
  std::memcpy(p, &v, sizeof(v));
}
// Saves or loads each field in turn.
template <typename... Fields>
void DoFields(PointerWrap& p, Fields&... fields)
{
  (p.Do(fields), ...);
}
}  // namespace

DSPManager::DSPManager(Core::System& system) : m_system(system)
{
}

DSPManager::~DSPManager() = default;

void DSPManager::DoState(PointerWrap& p)
{
  // Wii MEM2 is saved by the memory manager.
  if (!m_aram.wii_mode)
    p.DoArray(m_aram.ptr, m_aram.size);
  DoFields(p, m_dsp_control, m_audio_dma, m_aram_dma);
  DoFields(p, m_aram_info, m_aram_mode, m_aram_refresh, m_dsp_slice);

  m_dsp_emulator->DoState(p);
}

DSPEmulator* DSPManager::GetDSPEmulator()
{
  return m_dsp_emulator.get();
}

void DSPManager::Init(bool hle)
{
  Reinit(hle);
  auto& timing = m_system.GetCoreTiming();
  m_event_type_generate_dsp_interrupt = timing.RegisterEvent("DSPint", GlobalGenerateDSPInterrupt);
  m_event_type_complete_aram = timing.RegisterEvent("ARAMint", GlobalCompleteARAM);
}

void DSPManager::Reinit(bool hle)
{
  m_dsp_emulator = CreateDSPEmulator(m_system, hle);
  m_is_lle = m_dsp_emulator->IsLLE();

  if (m_system.IsWii())
  {
    auto& memory = m_system.GetMemory();
    m_aram = {true, memory.GetExRamSizeReal(), memory.GetExRamMask(), memory.GetEXRAM()};
  }
  else
  {
    m_aram = {false, ARAM_SIZE, ARAM_MASK, nullptr};
    m_aram.ptr = static_cast<u8*>(Common::AllocateMemoryPages(m_aram.size));
  }

  m_audio_dma = {};
  m_aram_dma = {};

  m_dsp_control.Hex = CONTROL_HALT;
  m_aram_info = 0;
  m_aram_mode = 1;       // controller ready
  m_aram_refresh = 156;  // refresh rate in MHz
}

void DSPManager::Shutdown()
{
  if (!m_aram.wii_mode)
  {
    Common::FreeMemoryPages(m_aram.ptr, m_aram.size);
    m_aram.ptr = nullptr;
  }

  m_dsp_emulator->Shutdown();
  m_dsp_emulator.reset();
}

// --- Registers ----------------------------------------------------------------------

u16 DSPManager::ReadMailHigh(bool cpu_mailbox)
{
  // Give an LLE DSP a little time, so that polling loops make progress.
  if (m_is_lle && m_dsp_slice > DSP_MAIL_SLICE)
  {
    m_dsp_emulator->DSP_Update(DSP_MAIL_SLICE);
    m_dsp_slice -= DSP_MAIL_SLICE;
  }
  return m_dsp_emulator->DSP_ReadMailBoxHigh(cpu_mailbox);
}

void DSPManager::WriteControl(u16 value)
{
  // The emulator owns the DSP side bits and may change them.
  const u16 from_emulator = m_dsp_emulator->DSP_WriteControlRegister(value);
  const u16 written = (value & ~DSP_CONTROL_MASK) | (from_emulator & DSP_CONTROL_MASK);

  // A DSP reset also stops audio DMA.
  if (value & CONTROL_RESET)
    m_audio_dma.control = 0;

  u16 control = (m_dsp_control.Hex & ~CONTROL_WRITABLE) | (written & CONTROL_WRITABLE);
  // Writing 1 to an interrupt status bit acknowledges it.
  control &= ~(written & ALL_INTERRUPTS);
  m_dsp_control.Hex = control;

  if (control & CONTROL_PADDING)
    PanicAlertFmt("DSP control register written with unknown bits set: {:08x}", value);

  UpdateInterrupts();
}

void DSPManager::WriteAudioDMAControl(u16 value)
{
  const bool was_enabled = (m_audio_dma.control & AUDIO_DMA_ENABLE) != 0;
  m_audio_dma.control = value;

  // While a transfer runs, new settings are picked up when it ends.
  if (was_enabled || !(value & AUDIO_DMA_ENABLE))
    return;

  m_audio_dma.current_source_address = m_audio_dma.source_address;
  m_audio_dma.remaining_blocks_count = value & AUDIO_DMA_BLOCKS;
  INFO_LOG_FMT(AUDIO_INTERFACE, "Audio DMA: {} blocks from {:#010x}", value & AUDIO_DMA_BLOCKS,
               m_audio_dma.source_address);
  m_system.GetCoreTiming().ScheduleEvent(AUDIO_DMA_START_DELAY,
                                         m_event_type_generate_dsp_interrupt, INT_AID);
}

void DSPManager::RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  // Plain halfword registers; a zero mask makes the register read-only.
  const struct
  {
    u32 offset;
    u16* field;
    u16 write_mask;
  } plain_registers[] = {
      {AR_MODE, &m_aram_mode, 0},
      {AR_INFO, &m_aram_info, MASK_AR_INFO},
      {AR_REFRESH, &m_aram_refresh, MASK_AR_REFRESH},
      {AR_DMA_MMADDR_H, High(&m_aram_dma.mm_address), MASK_ADDRESS_HIGH},
      {AR_DMA_MMADDR_L, Low(&m_aram_dma.mm_address), MASK_ADDRESS_LOW},
      {AR_DMA_ARADDR_H, High(&m_aram_dma.ar_address), MASK_ADDRESS_HIGH},
      {AR_DMA_ARADDR_L, Low(&m_aram_dma.ar_address), MASK_ADDRESS_LOW},
      {AR_DMA_CNT_H, High(&m_aram_dma.count), MASK_ADDRESS_HIGH | MASK_COUNT_DIRECTION},
      {AUDIO_DMA_START_LO, Low(&m_audio_dma.source_address), MASK_ADDRESS_LOW},
  };
  for (const auto& reg : plain_registers)
  {
    mmio->Register(base | reg.offset, MMIO::DirectRead<u16>(reg.field),
                   reg.write_mask ? MMIO::DirectWrite<u16>(reg.field, reg.write_mask) :
                                    MMIO::InvalidWrite<u16>());
  }

  // Mailboxes go to the DSP emulator.
  mmio->Register(base | DSP_MAIL_TO_DSP_HI, MMIO::ComplexRead<u16>([](Core::System& system, u32) {
                   return system.GetDSP().ReadMailHigh(true);
                 }),
                 MMIO::ComplexWrite<u16>([](Core::System& system, u32, u16 val) {
                   system.GetDSP().m_dsp_emulator->DSP_WriteMailBoxHigh(true, val);
                 }));
  mmio->Register(base | DSP_MAIL_TO_DSP_LO, MMIO::ComplexRead<u16>([](Core::System& system, u32) {
                   return system.GetDSP().m_dsp_emulator->DSP_ReadMailBoxLow(true);
                 }),
                 MMIO::ComplexWrite<u16>([](Core::System& system, u32, u16 val) {
                   system.GetDSP().m_dsp_emulator->DSP_WriteMailBoxLow(true, val);
                 }));
  mmio->Register(base | DSP_MAIL_FROM_DSP_HI,
                 MMIO::ComplexRead<u16>([](Core::System& system, u32) {
                   return system.GetDSP().ReadMailHigh(false);
                 }),
                 MMIO::InvalidWrite<u16>());
  mmio->Register(base | DSP_MAIL_FROM_DSP_LO,
                 MMIO::ComplexRead<u16>([](Core::System& system, u32) {
                   return system.GetDSP().m_dsp_emulator->DSP_ReadMailBoxLow(false);
                 }),
                 MMIO::InvalidWrite<u16>());

  mmio->Register(base | DSP_CONTROL, MMIO::ComplexRead<u16>([](Core::System& system, u32) {
                   auto& dsp = system.GetDSP();
                   const u16 dsp_bits =
                       dsp.m_dsp_emulator->DSP_ReadControlRegister() & DSP_CONTROL_MASK;
                   return static_cast<u16>((dsp.m_dsp_control.Hex & ~DSP_CONTROL_MASK) |
                                           dsp_bits);
                 }),
                 MMIO::ComplexWrite<u16>([](Core::System& system, u32, u16 val) {
                   system.GetDSP().WriteControl(val);
                 }));

  // Writing the low half of the count starts an ARAM DMA.
  mmio->Register(base | AR_DMA_CNT_L, MMIO::DirectRead<u16>(Low(&m_aram_dma.count)),
                 MMIO::ComplexWrite<u16>([](Core::System& system, u32, u16 val) {
                   auto& dsp = system.GetDSP();
                   *Low(&dsp.m_aram_dma.count) = val & MASK_ADDRESS_LOW;
                   dsp.Do_ARAM_DMA();
                 }));

  mmio->Register(base | AUDIO_DMA_START_HI,
                 MMIO::DirectRead<u16>(High(&m_audio_dma.source_address)),
                 MMIO::ComplexWrite<u16>([](Core::System& system, u32, u16 val) {
                   const u16 mask = system.IsWii() ? MASK_AUDIO_HIGH_WII : MASK_AUDIO_HIGH_GAMECUBE;
                   *High(&system.GetDSP().m_audio_dma.source_address) = val & mask;
                 }));

  mmio->Register(base | AUDIO_DMA_CONTROL_LEN, MMIO::DirectRead<u16>(&m_audio_dma.control),
                 MMIO::ComplexWrite<u16>([](Core::System& system, u32, u16 val) {
                   system.GetDSP().WriteAudioDMAControl(val);
                 }));

  // Blocks left, counting the current one as done (software waits for 0).
  mmio->Register(base | AUDIO_DMA_BLOCKS_LEFT,
                 MMIO::ComplexRead<u16>([](Core::System& system, u32) {
                   const u16 left = system.GetDSP().m_audio_dma.remaining_blocks_count;
                   return static_cast<u16>(left == 0 ? 0 : left - 1);
                 }),
                 MMIO::InvalidWrite<u16>());

  // Word accesses are split into two halfword accesses.
  for (u32 offset = 0; offset < 0x1000; offset += 4)
  {
    mmio->Register(base | offset,
                   MMIO::ReadToSmaller<u32>(mmio, base | offset, base | (offset + 2)),
                   MMIO::WriteToSmaller<u32>(mmio, base | offset, base | (offset + 2)));
  }
}

// --- Interrupts -----------------------------------------------------------------------

void DSPManager::UpdateInterrupts()
{
  // Each status bit's enable is the bit above it.
  const u16 control = m_dsp_control.Hex;
  const bool pending = (control & (control >> 1) & ALL_INTERRUPTS) != 0;
  m_system.GetProcessorInterface().SetInterrupt(ProcessorInterface::INT_CAUSE_DSP, pending);
}

void DSPManager::GlobalGenerateDSPInterrupt(Core::System& system, u64 DSPIntType, s64 cyclesLate)
{
  system.GetDSP().GenerateDSPInterrupt(DSPIntType, cyclesLate);
}

void DSPManager::GenerateDSPInterrupt(u64 DSPIntType, s64)
{
  m_dsp_control.Hex |= static_cast<u16>(DSPIntType & ALL_INTERRUPTS);
  UpdateInterrupts();
}

void DSPManager::GenerateDSPInterruptFromDSPEmu(DSPInterruptType type, int cycles_into_future)
{
  m_system.GetCoreTiming().ScheduleEvent(cycles_into_future, m_event_type_generate_dsp_interrupt,
                                         type, CoreTiming::FromThread::ANY);
}

void DSPManager::GlobalCompleteARAM(Core::System& system, u64 userdata, s64 cyclesLate)
{
  system.GetDSP().CompleteARAM(userdata, cyclesLate);
}

void DSPManager::CompleteARAM(u64, s64)
{
  m_dsp_control.Hex &= ~CONTROL_DMA_BUSY;
  GenerateDSPInterrupt(INT_ARAM, 0);
}

// --- Periodic work ---------------------------------------------------------------------

void DSPManager::UpdateDSPSlice(int cycles)
{
  if (!m_is_lle)
  {
    m_dsp_emulator->DSP_Update(cycles);
    return;
  }
  // Run what is left of the previous slice, then start the new one.
  m_dsp_emulator->DSP_Update(m_dsp_slice);
  m_dsp_slice = m_dsp_slice % 6 + cycles;
}

void DSPManager::UpdateAudioDMA()
{
  static short silence[8 * 2] = {};

  if (!(m_audio_dma.control & AUDIO_DMA_ENABLE))
  {
    AudioCommon::SendAIBuffer(m_system, silence, 8);
    return;
  }

  // One block: 8 stereo samples.
  auto& memory = m_system.GetMemory();
  const void* block = memory.GetPointerForRange(m_audio_dma.current_source_address, 32);
  AudioCommon::SendAIBuffer(m_system, static_cast<const short*>(block), 8);

  if (m_audio_dma.remaining_blocks_count != 0)
  {
    --m_audio_dma.remaining_blocks_count;
    m_audio_dma.current_source_address += 32;
  }

  // End of the buffer: latch the registers again and signal it.
  if (m_audio_dma.remaining_blocks_count == 0)
  {
    m_audio_dma.current_source_address = m_audio_dma.source_address;
    m_audio_dma.remaining_blocks_count = m_audio_dma.control & AUDIO_DMA_BLOCKS;
    GenerateDSPInterrupt(INT_AID, 0);
  }
}

// --- ARAM DMA --------------------------------------------------------------------------

// The whole transfer is done at once; only the completion interrupt is
// delayed. Addresses past the end of ARAM go to the high speed port (GameCube).
void DSPManager::Do_ARAM_DMA()
{
  auto& memory = m_system.GetMemory();
  AramDMA& dma = m_aram_dma;

  m_dsp_control.Hex |= CONTROL_DMA_BUSY;

  const auto bytes_left = [&] { return dma.count & ARAM_COUNT_BITS; };
  const auto consume = [&](u32 bytes) {
    dma.mm_address += bytes;
    dma.ar_address += bytes;
    dma.count = (dma.count & ARAM_TO_MAIN) | ((dma.count - bytes) & ARAM_COUNT_BITS);
  };

  const int cycles = static_cast<int>(bytes_left() / 32 * ARAM_DMA_CYCLES_PER_BLOCK);
  m_system.GetCoreTiming().ScheduleEvent(cycles, m_event_type_complete_aram);

  const bool to_main = (dma.count & ARAM_TO_MAIN) != 0;
  DEBUG_LOG_FMT(DSPINTERFACE, "ARAM DMA {:08x} bytes, ARAM {:08x} {} main {:08x}, PC {:08x}",
                bytes_left(), dma.ar_address, to_main ? "->" : "<-", dma.mm_address,
                m_system.GetPPCState().pc);

  dma.ar_address &= DMA_ADDRESS_WRAP;
  dma.mm_address &= DMA_ADDRESS_WRAP;

  if (dma.ar_address < m_aram.size)
  {
    // Copy in 8-byte units (hardware uses 32).
    const u16 configuration = m_aram_info & 0xf;
    while (bytes_left() != 0)
    {
      u8* aram = &m_aram.ptr[dma.ar_address & m_aram.mask];
      if (to_main)
      {
        memory.Write_U64_Swap(LoadU64(aram), dma.mm_address);
      }
      else
      {
        // In configuration 4 the low 4 MiB also show up 4 MiB higher.
        if (configuration == 4 && dma.ar_address < 0x400000)
        {
          StoreU64(&m_aram.ptr[(dma.ar_address + 0x400000) & m_aram.mask],
                   Common::swap64(memory.Read_U64(dma.mm_address)));
        }
        StoreU64(aram, Common::swap64(memory.Read_U64(dma.mm_address)));
      }
      consume(8);
    }
    return;
  }

  if (m_aram.wii_mode)
    return;

  u8* main = memory.GetPointerForRange(dma.mm_address, bytes_left());
  if (main == nullptr)
    return;

  auto& hsp = m_system.GetHSP();
  while (bytes_left() != 0)
  {
    if (to_main)
      hsp.Read(dma.ar_address, std::span<u8, HSP::TRANSFER_SIZE>(main, HSP::TRANSFER_SIZE));
    else
      hsp.Write(dma.ar_address, std::span<const u8, HSP::TRANSFER_SIZE>(main, HSP::TRANSFER_SIZE));
    consume(HSP::TRANSFER_SIZE);
    main += HSP::TRANSFER_SIZE;
  }
}

// --- ARAM access -----------------------------------------------------------------------

// On the Wii, DSP accesses below 0x10000000 reach MEM1.
u8 DSPManager::ReadARAM(u32 address) const
{
  if (m_aram.wii_mode && !(address & 0x10000000))
  {
    auto& memory = m_system.GetMemory();
    return memory.Read_U8(address & memory.GetRamMask());
  }
  return m_aram.ptr[address & m_aram.mask];
}

void DSPManager::WriteARAM(u8 value, u32 address)
{
  m_aram.ptr[address & m_aram.mask] = value;
}

u8* DSPManager::GetARAMPtr() const
{
  return m_aram.ptr;
}

u32 DSPManager::GetARAMSize() const
{
  return m_aram.size;
}

}  // namespace DSP
