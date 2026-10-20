// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/CommandProcessor.h"

#include <atomic>
#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Flag.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/Fifo.h"

namespace CommandProcessor
{
namespace
{
constexpr auto RELAXED = std::memory_order_relaxed;

// With a separate GPU thread the CPU must not touch GPU-owned state directly.
bool GpuOnOwnThread(const Core::System& system)
{
  return system.IsDualCoreMode();
}

void OnInterruptEvent(Core::System& system, u64 userdata, s64)
{
  system.GetCommandProcessor().UpdateInterrupts(userdata);
}

// Replaces the upper half of a 32-bit register, leaving the lower half alone.
void StoreHighHalf(std::atomic<u32>& reg, u16 high)
{
  const u32 low = reg.load(RELAXED) & 0x0000FFFFu;
  reg.store(low | (u32{high} << 16), RELAXED);
}

// Bytes queued between the GPU's published read pointer and the write pointer, allowing for the
// write pointer having wrapped around to the start of the FIFO window.
u32 QueuedFromSafeReadPointer(const SCPFifoStruct& fifo)
{
  const u32 write = fifo.CPWritePointer.load(RELAXED);
  const u32 read = fifo.SafeCPReadPointer.load(RELAXED);
  if (write >= read)
    return write - read;
  return fifo.CPEnd.load(RELAXED) - read + write - fifo.CPBase.load(RELAXED) + 32;
}

// One-bit status fields take the flag's lowest bit.
u16 LowBit(u32 flag)
{
  return static_cast<u16>(flag & 1);
}

const char* OnOff(bool b)
{
  return b ? "ON" : "OFF";
}

const char* TrueFalse(u32 b)
{
  return b ? "true" : "false";
}
}  // namespace

void SCPFifoStruct::Init()
{
  for (std::atomic<u32>* reg :
       {&CPBase, &CPEnd, &CPReadWriteDistance, &CPWritePointer, &CPReadPointer, &CPBreakpoint,
        &SafeCPReadPointer, &bFF_GPLinkEnable, &bFF_GPReadEnable, &bFF_BPEnable, &bFF_BPInt})
  {
    reg->store(0);
  }
  CPHiWatermark = 0;
  CPLoWatermark = 0;

  for (std::atomic<u32>* flag : {&bFF_Breakpoint, &bFF_HiWatermark, &bFF_HiWatermarkInt,
                                 &bFF_LoWatermark, &bFF_LoWatermarkInt})
  {
    flag->store(0, RELAXED);
  }
}

void SCPFifoStruct::DoState(PointerWrap& p)
{
  p.Do(CPBase);
  p.Do(CPEnd);
  p.Do(CPHiWatermark);
  p.Do(CPLoWatermark);
  p.Do(CPReadWriteDistance);
  p.Do(CPWritePointer);
  p.Do(CPReadPointer);
  p.Do(CPBreakpoint);
  p.Do(SafeCPReadPointer);

  p.Do(bFF_GPLinkEnable);
  p.Do(bFF_GPReadEnable);
  p.Do(bFF_BPEnable);
  p.Do(bFF_BPInt);
  p.Do(bFF_Breakpoint);

  p.Do(bFF_LoWatermarkInt);
  p.Do(bFF_HiWatermarkInt);

  p.Do(bFF_LoWatermark);
  p.Do(bFF_HiWatermark);
}

CommandProcessorManager::CommandProcessorManager(Core::System& system) : m_system{system}
{
}

void CommandProcessorManager::DoState(PointerWrap& p)
{
  p.Do(m_status);
  p.Do(m_control);
  p.Do(m_clear);
  p.Do(m_perf_select);
  p.Do(m_unknown_0a);
  m_fifo.DoState(p);

  p.Do(m_irq_asserted);
  p.Do(m_irq_update_pending);
}

void CommandProcessorManager::Init()
{
  // Power-on: nothing queued, so both idle bits read as set.
  m_status.Hex = 0;
  m_status.ReadIdle = 1;
  m_status.CommandIdle = 1;
  m_control.Hex = 0;
  m_clear.Hex = 0;

  m_fifo.Init();
  m_reported_bad_opcode = false;
  m_irq_asserted.Clear();
  m_irq_update_pending.Clear();

  m_interrupt_event = m_system.GetCoreTiming().RegisterEvent("CPInterrupt", OnInterruptEvent);
}

void CommandProcessorManager::RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  // FIFO address registers ignore some bits: the low halves are 32-byte aligned and the high
  // halves are limited to the platform's physical address width.
  constexpr u16 LOW_HALF_MASK = 0xffe0;
  const u16 high_half_mask = static_cast<u16>(GetPhysicalAddressMask(m_system.IsWii()) >> 16);

  // Plain storage register pairs (the read/write distance and read pointer are handled below).
  const auto map_plain = [&](u32 low_offset, u16* low, u16* high) {
    mmio->Register(base | low_offset, MMIO::DirectRead<u16>(low),
                   MMIO::DirectWrite<u16>(low, LOW_HALF_MASK));
    mmio->Register(base | (low_offset + 2), MMIO::DirectRead<u16>(high),
                   MMIO::DirectWrite<u16>(high, high_half_mask));
  };
  map_plain(FIFO_BASE_LO, MMIO::Utils::LowPart(&m_fifo.CPBase),
            MMIO::Utils::HighPart(&m_fifo.CPBase));
  map_plain(FIFO_END_LO, MMIO::Utils::LowPart(&m_fifo.CPEnd), MMIO::Utils::HighPart(&m_fifo.CPEnd));
  map_plain(FIFO_HI_WATERMARK_LO, MMIO::Utils::LowPart(&m_fifo.CPHiWatermark),
            MMIO::Utils::HighPart(&m_fifo.CPHiWatermark));
  map_plain(FIFO_LO_WATERMARK_LO, MMIO::Utils::LowPart(&m_fifo.CPLoWatermark),
            MMIO::Utils::HighPart(&m_fifo.CPLoWatermark));
  map_plain(FIFO_WRITE_POINTER_LO, MMIO::Utils::LowPart(&m_fifo.CPWritePointer),
            MMIO::Utils::HighPart(&m_fifo.CPWritePointer));
  map_plain(FIFO_BP_LO, MMIO::Utils::LowPart(&m_fifo.CPBreakpoint),
            MMIO::Utils::HighPart(&m_fifo.CPBreakpoint));

  // Performance metric registers are not emulated; they read as fixed values.
  for (const u32 offset :
       {XF_RASBUSY_L, XF_RASBUSY_H, XF_CLKS_L, XF_CLKS_H, XF_WAIT_IN_L, XF_WAIT_IN_H,
        XF_WAIT_OUT_L, XF_WAIT_OUT_H, VCACHE_METRIC_CHECK_L, VCACHE_METRIC_CHECK_H,
        VCACHE_METRIC_MISS_L, VCACHE_METRIC_MISS_H, VCACHE_METRIC_STALL_L, VCACHE_METRIC_STALL_H})
  {
    mmio->Register(base | offset, MMIO::Constant<u16>(0), MMIO::InvalidWrite<u16>());
  }
  mmio->Register(base | CLKS_PER_VTX_OUT, MMIO::Constant<u16>(4), MMIO::InvalidWrite<u16>());

  mmio->Register(base | STATUS_REGISTER, MMIO::ComplexRead<u16>([](Core::System& sys, u32) {
                   CommandProcessorManager& cp = sys.GetCommandProcessor();
                   sys.GetFifo().SyncGPUForRegisterAccess();
                   cp.SetCpStatusRegister();
                   return cp.m_status.Hex;
                 }),
                 MMIO::InvalidWrite<u16>());

  mmio->Register(base | CTRL_REGISTER, MMIO::DirectRead<u16>(&m_control.Hex),
                 MMIO::ComplexWrite<u16>([](Core::System& sys, u32, u16 val) {
                   CommandProcessorManager& cp = sys.GetCommandProcessor();
                   cp.m_control.Hex = val & 0x3F;
                   cp.SetCpControlRegister();
                   sys.GetFifo().RunGpu();
                 }));

  mmio->Register(base | CLEAR_REGISTER, MMIO::Constant<u16>(0),
                 MMIO::ComplexWrite<u16>([](Core::System& sys, u32, u16 val) {
                   CommandProcessorManager& cp = sys.GetCommandProcessor();
                   cp.m_clear.Hex = val;
                   cp.SetCpClearRegister();
                   sys.GetFifo().RunGpu();
                 }));

  // Two registers whose purpose is not understood (the first is written by GXSetGPMetric); they
  // just store the bits that are known to stick.
  mmio->Register(base | PERF_SELECT, MMIO::DirectRead<u16>(&m_perf_select),
                 MMIO::DirectWrite<u16>(&m_perf_select, 0x0007));
  mmio->Register(base | UNK_0A_REGISTER, MMIO::DirectRead<u16>(&m_unknown_0a),
                 MMIO::DirectWrite<u16>(&m_unknown_0a, 0x00FF));

  // The read/write distance and read pointer are owned by the GPU thread in dual-core mode, so
  // the CPU derives them from the published read pointer there.
  const bool threaded = GpuOnOwnThread(m_system);

  MMIO::ReadHandlingMethod<u16>* distance_low_read;
  MMIO::ReadHandlingMethod<u16>* distance_high_read;
  if (threaded)
  {
    distance_low_read = MMIO::ComplexRead<u16>([](Core::System& sys, u32) {
      return static_cast<u16>(QueuedFromSafeReadPointer(sys.GetCommandProcessor().GetFifo()));
    });
    distance_high_read = MMIO::ComplexRead<u16>([](Core::System& sys, u32) {
      const SCPFifoStruct& fifo = sys.GetCommandProcessor().GetFifo();
      sys.GetFifo().SyncGPUForRegisterAccess();
      return static_cast<u16>(QueuedFromSafeReadPointer(fifo) >> 16);
    });
  }
  else
  {
    distance_low_read = MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&m_fifo.CPReadWriteDistance));
    distance_high_read = MMIO::ComplexRead<u16>([](Core::System& sys, u32) {
      const SCPFifoStruct& fifo = sys.GetCommandProcessor().GetFifo();
      sys.GetFifo().SyncGPUForRegisterAccess();
      return static_cast<u16>(fifo.CPReadWriteDistance.load(RELAXED) >> 16);
    });
  }
  mmio->Register(
      base | FIFO_RW_DISTANCE_LO, distance_low_read,
      MMIO::DirectWrite<u16>(MMIO::Utils::LowPart(&m_fifo.CPReadWriteDistance), LOW_HALF_MASK));
  mmio->Register(base | FIFO_RW_DISTANCE_HI, distance_high_read,
                 MMIO::ComplexWrite<u16>([high_half_mask](Core::System& sys, u32, u16 val) {
                   SCPFifoStruct& fifo = sys.GetCommandProcessor().GetFifo();
                   sys.GetFifo().SyncGPUForRegisterAccess();
                   StoreHighHalf(fifo.CPReadWriteDistance, val & high_half_mask);
                   sys.GetFifo().RunGpu();
                 }));

  std::atomic<u32>* const visible_read_pointer =
      threaded ? &m_fifo.SafeCPReadPointer : &m_fifo.CPReadPointer;
  mmio->Register(
      base | FIFO_READ_POINTER_LO, MMIO::DirectRead<u16>(MMIO::Utils::LowPart(visible_read_pointer)),
      MMIO::DirectWrite<u16>(MMIO::Utils::LowPart(&m_fifo.CPReadPointer), LOW_HALF_MASK));

  MMIO::ReadHandlingMethod<u16>* read_pointer_high_read;
  MMIO::WriteHandlingMethod<u16>* read_pointer_high_write;
  if (threaded)
  {
    read_pointer_high_read = MMIO::ComplexRead<u16>([](Core::System& sys, u32) {
      const SCPFifoStruct& fifo = sys.GetCommandProcessor().GetFifo();
      sys.GetFifo().SyncGPUForRegisterAccess();
      return static_cast<u16>(fifo.SafeCPReadPointer.load(RELAXED) >> 16);
    });
    read_pointer_high_write =
        MMIO::ComplexWrite<u16>([high_half_mask](Core::System& sys, u32, u16 val) {
          SCPFifoStruct& fifo = sys.GetCommandProcessor().GetFifo();
          sys.GetFifo().SyncGPUForRegisterAccess();
          StoreHighHalf(fifo.CPReadPointer, val & high_half_mask);
          // Republish so the CPU-side view follows the write immediately.
          fifo.SafeCPReadPointer.store(fifo.CPReadPointer.load(RELAXED), RELAXED);
        });
  }
  else
  {
    read_pointer_high_read = MMIO::ComplexRead<u16>([](Core::System& sys, u32) {
      const SCPFifoStruct& fifo = sys.GetCommandProcessor().GetFifo();
      sys.GetFifo().SyncGPUForRegisterAccess();
      return static_cast<u16>(fifo.CPReadPointer.load(RELAXED) >> 16);
    });
    read_pointer_high_write =
        MMIO::ComplexWrite<u16>([high_half_mask](Core::System& sys, u32, u16 val) {
          SCPFifoStruct& fifo = sys.GetCommandProcessor().GetFifo();
          sys.GetFifo().SyncGPUForRegisterAccess();
          StoreHighHalf(fifo.CPReadPointer, val & high_half_mask);
        });
  }
  mmio->Register(base | FIFO_READ_POINTER_HI, read_pointer_high_read, read_pointer_high_write);
}

void CommandProcessorManager::GatherPipeBursted()
{
  SetCPStatusFromCPU();

  ProcessorInterface::ProcessorInterfaceManager& pi = m_system.GetProcessorInterface();
  Fifo::FifoManager& gpu_fifo = m_system.GetFifo();

  if (!m_control.GPLinkEnable)
  {
    // Unlinked: the burst went to a FIFO the GPU is not reading from. A game may still be
    // filling the very FIFO attached to the GPU (multi-buffering, e.g. Pokemon XD); with a
    // free-running GPU thread let it finish that one first.
    if (GpuOnOwnThread(m_system) && !gpu_fifo.UseDeterministicGPUThread())
    {
      const bool same_window = pi.m_fifo_cpu_end == m_fifo.CPEnd.load(RELAXED) &&
                               pi.m_fifo_cpu_base == m_fifo.CPBase.load(RELAXED);
      if (same_window && m_fifo.CPReadWriteDistance.load(RELAXED) > 0)
        gpu_fifo.FlushGpu();
    }
    gpu_fifo.RunGpu();
    return;
  }

  // Advance the write pointer by one burst, wrapping at the end of the window.
  if (m_fifo.CPWritePointer.load(RELAXED) == m_fifo.CPEnd.load(RELAXED))
    m_fifo.CPWritePointer.store(m_fifo.CPBase, RELAXED);
  else
    m_fifo.CPWritePointer.fetch_add(GPFifo::GATHER_PIPE_SIZE, RELAXED);

  if (m_control.GPReadEnable && m_control.GPLinkEnable)
  {
    pi.m_fifo_cpu_write_pointer = m_fifo.CPWritePointer.load(RELAXED);
    pi.m_fifo_cpu_base = m_fifo.CPBase.load(RELAXED);
    pi.m_fifo_cpu_end = m_fifo.CPEnd.load(RELAXED);
  }

  // Close to overflowing: have the CPU check for the watermark interrupt sooner.
  if (m_fifo.bFF_HiWatermark.load(RELAXED) != 0)
    m_system.GetCoreTiming().ForceExceptionCheck(0);

  m_fifo.CPReadWriteDistance.fetch_add(GPFifo::GATHER_PIPE_SIZE, std::memory_order_seq_cst);

  gpu_fifo.RunGpu();

  ASSERT_MSG(COMMANDPROCESSOR,
             m_fifo.CPReadWriteDistance.load(RELAXED) <=
                 m_fifo.CPEnd.load(RELAXED) - m_fifo.CPBase.load(RELAXED),
             "FIFO is overflowed by GatherPipe !\nCPU thread is too fast!");

  ASSERT_MSG(COMMANDPROCESSOR, m_fifo.CPWritePointer.load(RELAXED) == pi.m_fifo_cpu_write_pointer,
             "FIFOs linked but out of sync");
  ASSERT_MSG(COMMANDPROCESSOR, m_fifo.CPBase.load(RELAXED) == pi.m_fifo_cpu_base,
             "FIFOs linked but out of sync");
  ASSERT_MSG(COMMANDPROCESSOR, m_fifo.CPEnd.load(RELAXED) == pi.m_fifo_cpu_end,
             "FIFOs linked but out of sync");
}

void CommandProcessorManager::UpdateInterrupts(u64 userdata)
{
  const bool assert_line = userdata != 0;
  if (assert_line)
  {
    m_irq_asserted.Set();
    DEBUG_LOG_FMT(COMMANDPROCESSOR, "Interrupt set");
  }
  else
  {
    m_irq_asserted.Clear();
    DEBUG_LOG_FMT(COMMANDPROCESSOR, "Interrupt cleared");
  }
  m_system.GetProcessorInterface().SetInterrupt(INT_CAUSE_CP, assert_line);
  m_system.GetCoreTiming().ForceExceptionCheck(0);
  m_irq_update_pending.Clear();
  m_system.GetFifo().RunGpu();
}

void CommandProcessorManager::UpdateInterruptsFromVideoBackend(u64 userdata)
{
  if (m_system.GetFifo().UseDeterministicGPUThread())
    return;
  m_system.GetCoreTiming().ScheduleEvent(0, m_interrupt_event, userdata,
                                         CoreTiming::FromThread::NON_CPU);
}

bool CommandProcessorManager::IsInterruptWaiting() const
{
  return m_irq_update_pending.IsSet();
}

CommandProcessorManager::InterruptSources CommandProcessorManager::RefreshWatermarks()
{
  m_fifo.bFF_HiWatermark.store(m_fifo.CPReadWriteDistance.load(RELAXED) > m_fifo.CPHiWatermark,
                               RELAXED);
  m_fifo.bFF_LoWatermark.store(m_fifo.CPReadWriteDistance.load(RELAXED) < m_fifo.CPLoWatermark,
                               RELAXED);

  InterruptSources src;
  src.breakpoint = m_fifo.bFF_Breakpoint.load(RELAXED) && m_fifo.bFF_BPInt.load(RELAXED);
  src.overflow = m_fifo.bFF_HiWatermark.load(RELAXED) && m_fifo.bFF_HiWatermarkInt.load(RELAXED);
  src.underflow = m_fifo.bFF_LoWatermark.load(RELAXED) && m_fifo.bFF_LoWatermarkInt.load(RELAXED);
  src.any_enabled = (src.breakpoint || src.overflow || src.underflow) && m_control.GPReadEnable;
  return src;
}

void CommandProcessorManager::SetCPStatusFromGPU()
{
  // Track whether the GPU's read pointer sits on the breakpoint.
  const bool was_at_breakpoint = m_fifo.bFF_Breakpoint.load(RELAXED) != 0;
  const bool at_breakpoint =
      m_fifo.bFF_BPEnable.load(RELAXED) != 0 &&
      m_fifo.CPBreakpoint.load(RELAXED) == m_fifo.CPReadPointer.load(RELAXED);
  if (at_breakpoint && !was_at_breakpoint)
  {
    DEBUG_LOG_FMT(COMMANDPROCESSOR, "Hit breakpoint at {}", m_fifo.CPReadPointer.load(RELAXED));
    m_fifo.bFF_Breakpoint.store(1, RELAXED);
  }
  else if (!at_breakpoint && was_at_breakpoint)
  {
    DEBUG_LOG_FMT(COMMANDPROCESSOR, "Cleared breakpoint at {}",
                  m_fifo.CPReadPointer.load(RELAXED));
    m_fifo.bFF_Breakpoint.store(0, RELAXED);
  }

  const InterruptSources src = RefreshWatermarks();
  if (src.any_enabled == m_irq_asserted.IsSet() || m_irq_update_pending.IsSet())
    return;

  const u64 userdata = src.any_enabled ? 1 : 0;
  if (!GpuOnOwnThread(m_system))
  {
    UpdateInterrupts(userdata);
    return;
  }
  // From the GPU thread the line is changed on the CPU thread, via an event.
  m_irq_update_pending.Set();
  UpdateInterruptsFromVideoBackend(userdata);
}

void CommandProcessorManager::SetCPStatusFromCPU()
{
  const InterruptSources src = RefreshWatermarks();
  if (src.any_enabled == m_irq_asserted.IsSet() || m_irq_update_pending.IsSet())
    return;

  if (!GpuOnOwnThread(m_system))
  {
    UpdateInterrupts(src.any_enabled ? 1 : 0);
    return;
  }
  // Already on the CPU thread: change the line directly.
  m_irq_asserted.Set(src.any_enabled);
  DEBUG_LOG_FMT(COMMANDPROCESSOR, "Interrupt set");
  m_system.GetProcessorInterface().SetInterrupt(INT_CAUSE_CP, src.any_enabled);
}

void CommandProcessorManager::SetCpStatusRegister()
{
  // A FIFO is always attached to the GPU, so the status reflects m_fifo.
  const u32 queued = m_fifo.CPReadWriteDistance.load(RELAXED);
  m_status.Breakpoint = LowBit(m_fifo.bFF_Breakpoint.load(RELAXED));
  m_status.ReadIdle =
      !queued || m_fifo.CPReadPointer.load(RELAXED) == m_fifo.CPWritePointer.load(RELAXED);
  m_status.CommandIdle =
      !m_fifo.CPReadWriteDistance.load(RELAXED) || Fifo::AtBreakpoint(m_system) ||
      !m_fifo.bFF_GPReadEnable.load(RELAXED);
  m_status.UnderflowLoWatermark = LowBit(m_fifo.bFF_LoWatermark.load(RELAXED));
  m_status.OverflowHiWatermark = LowBit(m_fifo.bFF_HiWatermark.load(RELAXED));

  DEBUG_LOG_FMT(COMMANDPROCESSOR, "\t Read from STATUS_REGISTER : {:04x}", m_status.Hex);
  DEBUG_LOG_FMT(COMMANDPROCESSOR,
                "(r) status: iBP {} | fReadIdle {} | fCmdIdle {} | iOvF {} | iUndF {}",
                OnOff(m_status.Breakpoint), OnOff(m_status.ReadIdle), OnOff(m_status.CommandIdle),
                OnOff(m_status.OverflowHiWatermark), OnOff(m_status.UnderflowLoWatermark));
}

void CommandProcessorManager::SetCpControlRegister()
{
  // If reads are being switched off, let the GPU drain what it can first.
  if (m_fifo.bFF_GPReadEnable.load(RELAXED) && !m_control.GPReadEnable)
    m_system.GetFifo().SyncGPUForRegisterAccess();

  m_fifo.bFF_GPReadEnable.store(m_control.GPReadEnable, RELAXED);
  m_fifo.bFF_BPInt.store(m_control.BPInt, RELAXED);
  m_fifo.bFF_BPEnable.store(m_control.BPEnable, RELAXED);
  m_fifo.bFF_HiWatermarkInt.store(m_control.FifoOverflowIntEnable, RELAXED);
  m_fifo.bFF_LoWatermarkInt.store(m_control.FifoUnderflowIntEnable, RELAXED);
  m_fifo.bFF_GPLinkEnable.store(m_control.GPLinkEnable, RELAXED);

  DEBUG_LOG_FMT(COMMANDPROCESSOR, "\t GPREAD {} | BP {} | Int {} | OvF {} | UndF {} | LINK {}",
                OnOff(m_fifo.bFF_GPReadEnable.load(RELAXED)),
                OnOff(m_fifo.bFF_BPEnable.load(RELAXED)), OnOff(m_fifo.bFF_BPInt.load(RELAXED)),
                OnOff(m_control.FifoOverflowIntEnable), OnOff(m_control.FifoUnderflowIntEnable),
                OnOff(m_control.GPLinkEnable));
}

// Clearing the overflow/underflow latches and metrics is deliberately a no-op: GP timing is not
// modelled, so there is nothing for it to reset.
void CommandProcessorManager::SetCpClearRegister()
{
}

void CommandProcessorManager::ResetFifo()
{
  // Linked, reads disabled, no interrupts.
  m_control.Hex = 0x0010;
  SetCpControlRegister();
  m_perf_select = 0;
  m_unknown_0a = 0;
  m_fifo.CPLoWatermark = 0;
  m_fifo.CPHiWatermark = GetPhysicalAddressMask(m_system.IsWii()) & ~31u;
  SetCpStatusRegister();
}

void CommandProcessorManager::HandleUnknownOpcode(u8 cmd_byte, const u8* buffer, bool preprocess)
{
  // Opcodes 0x01-0x07 and 0x3f turn up in shipped software through harmless bugs (stray vertex
  // data, or a float 1.0 parsed as a command) and behave as no-ops on hardware, so they only get
  // a warning. Anything else hangs a real console and is reported loudly, once.
  const bool known_harmless = cmd_byte <= 0x7 || cmd_byte == 0x3f;
  const Common::Log::LogLevel level =
      known_harmless ? Common::Log::LogLevel::LWARNING : Common::Log::LogLevel::LERROR;

  // PC/LR are only approximate: the gather pipe and the GPU thread delay command delivery.
  const PowerPC::PowerPCState& ppc = m_system.GetPPCState();
  GENERIC_LOG_FMT(
      Common::Log::LogType::VIDEO, level,
      "FIFO: Unknown Opcode {:#04x} @ {}, preprocessing = {}, CPBase: {:#010x}, CPEnd: "
      "{:#010x}, CPHiWatermark: {:#010x}, CPLoWatermark: {:#010x}, CPReadWriteDistance: "
      "{:#010x}, CPWritePointer: {:#010x}, CPReadPointer: {:#010x}, CPBreakpoint: "
      "{:#010x}, bFF_GPReadEnable: {}, bFF_BPEnable: {}, bFF_BPInt: {}, bFF_Breakpoint: "
      "{}, bFF_GPLinkEnable: {}, bFF_HiWatermarkInt: {}, bFF_LoWatermarkInt: {}, "
      "approximate PC: {:08x}, approximate LR: {:08x}",
      cmd_byte, fmt::ptr(buffer), preprocess ? "yes" : "no", m_fifo.CPBase.load(RELAXED),
      m_fifo.CPEnd.load(RELAXED), m_fifo.CPHiWatermark, m_fifo.CPLoWatermark,
      m_fifo.CPReadWriteDistance.load(RELAXED), m_fifo.CPWritePointer.load(RELAXED),
      m_fifo.CPReadPointer.load(RELAXED), m_fifo.CPBreakpoint.load(RELAXED),
      TrueFalse(m_fifo.bFF_GPReadEnable.load(RELAXED)), TrueFalse(m_fifo.bFF_BPEnable.load(RELAXED)),
      TrueFalse(m_fifo.bFF_BPInt.load(RELAXED)), TrueFalse(m_fifo.bFF_Breakpoint.load(RELAXED)),
      TrueFalse(m_fifo.bFF_GPLinkEnable.load(RELAXED)),
      TrueFalse(m_fifo.bFF_HiWatermarkInt.load(RELAXED)),
      TrueFalse(m_fifo.bFF_LoWatermarkInt.load(RELAXED)), ppc.pc, LR(ppc));

  if (known_harmless || m_reported_bad_opcode)
    return;
  m_reported_bad_opcode = true;

  // Tailor the advice to the settings that make a GPU/CPU desync likely.
  const char* advice;
  const bool free_running_gpu =
      GpuOnOwnThread(m_system) && !m_system.GetFifo().UseDeterministicGPUThread();
  if (!free_running_gpu)
  {
    advice = _trans(
        "This error is usually caused by the emulated GPU desyncing with the emulated CPU, "
        "but your current settings make this unlikely to happen. The error may also be caused by "
        "a game bug.");
  }
  else if (m_system.GetCoreTiming().UseSyncOnSkipIdle() || m_system.GetFifo().UseSyncGPU())
  {
    advice = _trans(
        "This error is usually caused by the emulated GPU desyncing with the emulated CPU. "
        "Turn off the \"Dual Core\" setting to avoid this.");
  }
  else
  {
#ifdef ANDROID
    // Android names the option in its settings UI; that text is not translated yet.
    advice = "Please change the \"Synchronize GPU Thread\" setting to \"On Idle Skipping\"! "
             "It's currently set to \"Never\", which makes this problem very likely to happen.";
#else
    // i18n: Please leave SyncOnSkipIdle and True untranslated.
    // The user needs to enter these terms as-is in an INI file.
    advice = _trans("Please change the \"SyncOnSkipIdle\" setting to \"True\"! "
                    "It's currently disabled, which makes this problem very likely to happen.");
#endif
  }

  PanicAlertFmtT("GFX FIFO: Unknown Opcode ({0:#04x} @ {1}, preprocess={2}).\n"
                 "\n"
                 "{3}\n"
                 "\n"
                 "Further errors will be sent to the Video Backend log and "
                 "Dolphin will now likely crash or hang.",
                 cmd_byte, fmt::ptr(buffer), preprocess, Common::GetStringT(advice));
}

}  // namespace CommandProcessor
