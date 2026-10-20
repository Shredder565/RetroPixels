// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/ProcessorInterface.h"

#include <memory>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/SystemTimers.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/STM/STM.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"

namespace ProcessorInterface
{
namespace
{
// Flipper revision reported to software (revision C, as on retail units).
constexpr u32 FLIPPER_REVISION = 0x246500B1;

constexpr u32 INITIAL_UNKNOWN = 0x000001FF;
constexpr u32 INITIAL_BUS_STRENGTH = 0x02492492;

// Writable bits of the plain read/write registers.
constexpr u32 ERROR_CAUSE_BITS = 0x00000007;
constexpr u32 UNKNOWN_BITS = 0x000003FF;
constexpr u32 BUS_STRENGTH_BITS = 0x07FFFFFF;

// The FIFO pointers are 32-byte aligned physical addresses.
u32 FifoPointerMask(const Core::System& system)
{
  return CommandProcessor::GetPhysicalAddressMask(system.IsWii()) & ~u32(0x1F);
}

const char* CauseName(u32 cause)
{
  switch (cause)
  {
  case INT_CAUSE_PI:
    return "PI";
  case INT_CAUSE_RSW:
    return "RSW";
  case INT_CAUSE_DI:
    return "DI";
  case INT_CAUSE_SI:
    return "SI";
  case INT_CAUSE_EXI:
    return "EXI";
  case INT_CAUSE_AI:
    return "AI";
  case INT_CAUSE_DSP:
    return "DSP";
  case INT_CAUSE_MEMORY:
    return "MEMORY";
  case INT_CAUSE_VI:
    return "VI";
  case INT_CAUSE_PE_TOKEN:
    return "PE_TOKEN";
  case INT_CAUSE_PE_FINISH:
    return "PE_FINISH";
  case INT_CAUSE_CP:
    return "CP";
  case INT_CAUSE_DEBUG:
    return "DEBUG";
  case INT_CAUSE_HSP:
    return "HSP";
  case INT_CAUSE_WII_IPC:
    return "WII_IPC";
  case INT_CAUSE_RST_BUTTON:
    return "RST_BUTTON";
  default:
    return "(several or unknown)";
  }
}

void NotifyEventHook(Core::System& system, bool power)
{
  const auto ios = system.GetIOS();
  if (!ios)
    return;
  const auto device = ios->GetDeviceByName("/dev/stm/eventhook");
  if (!device)
    return;
  auto& hook = static_cast<IOS::HLE::STMEventHookDevice&>(*device);
  if (power)
    hook.PowerButton();
  else
    hook.ResetButton();
}
}  // namespace

ProcessorInterfaceManager::ProcessorInterfaceManager(Core::System& system) : m_system(system)
{
}

ProcessorInterfaceManager::~ProcessorInterfaceManager() = default;

void ProcessorInterfaceManager::DoState(PointerWrap& p)
{
  for (u32* field : {&m_interrupt_mask, &m_interrupt_cause, &m_fifo_cpu_base, &m_fifo_cpu_end,
                     &m_fifo_cpu_write_pointer, &m_error_cause, &m_error_address, &m_reset_code,
                     &m_unknown, &m_flipper_bus_strength})
  {
    p.Do(*field);
  }
}

void ProcessorInterfaceManager::Init()
{
  // The reset button starts released and a VI interrupt is pending at boot.
  m_interrupt_cause = INT_CAUSE_RST_BUTTON | INT_CAUSE_VI;
  m_interrupt_mask = 0;
  m_fifo_cpu_base = m_fifo_cpu_end = m_fifo_cpu_write_pointer = 0;
  m_error_cause = m_error_address = 0;
  m_reset_code = 0;  // cold boot
  m_unknown = INITIAL_UNKNOWN;
  m_flipper_bus_strength = INITIAL_BUS_STRENGTH;

  auto& timing = m_system.GetCoreTiming();
  m_event_type_toggle_reset_button =
      timing.RegisterEvent("ToggleResetButton", ToggleResetButtonCallback);
  m_event_type_ios_notify_reset_button =
      timing.RegisterEvent("IOSNotifyResetButton", IOSNotifyResetButtonCallback);
  m_event_type_ios_notify_power_button =
      timing.RegisterEvent("IOSNotifyPowerButton", IOSNotifyPowerButtonCallback);
}

void ProcessorInterfaceManager::RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  // Interrupt cause: writing 1 acknowledges a cause bit.
  mmio->Register(base | PI_INTERRUPT_CAUSE, MMIO::DirectRead<u32>(&m_interrupt_cause),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& pi = system.GetProcessorInterface();
                   pi.m_interrupt_cause &= ~val;
                   pi.UpdateException();
                 }));

  mmio->Register(base | PI_INTERRUPT_MASK, MMIO::DirectRead<u32>(&m_interrupt_mask),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& pi = system.GetProcessorInterface();
                   pi.m_interrupt_mask = val;
                   pi.UpdateException();
                 }));

  // FIFO pointers.
  const std::pair<u32, u32 ProcessorInterfaceManager::*> fifo_registers[] = {
      {PI_FIFO_BASE, &ProcessorInterfaceManager::m_fifo_cpu_base},
      {PI_FIFO_END, &ProcessorInterfaceManager::m_fifo_cpu_end},
      {PI_FIFO_WPTR, &ProcessorInterfaceManager::m_fifo_cpu_write_pointer},
  };
  for (const auto& [offset, member] : fifo_registers)
  {
    mmio->Register(base | offset, MMIO::DirectRead<u32>(&(this->*member)),
                   MMIO::ComplexWrite<u32>([member](Core::System& system, u32, u32 val) {
                     system.GetProcessorInterface().*member = val & FifoPointerMask(system);
                   }));
  }

  // FIFO reset (GXAbortFrame).
  mmio->Register(base | PI_FIFO_RESET, MMIO::InvalidRead<u32>(),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   INFO_LOG_FMT(PROCESSORINTERFACE, "PI_FIFO_RESET <- {:08x}", val);
                   if ((val & 1) == 0)
                     return;
                   system.GetGPFifo().ResetGatherPipe();
                   system.GetCommandProcessor().ResetFifo();
                   // The video buffer belongs to the GPU thread, so its reset goes
                   // through the request queue (which runs it at once in single
                   // core mode).
                   AsyncRequests::GetInstance()->PushEvent(
                       [] { Core::System::GetInstance().GetFifo().ResetVideoBuffer(); });
                 }));

  mmio->Register(base | PI_ERROR_CAUSE, MMIO::DirectRead<u32>(&m_error_cause),
                 MMIO::DirectWrite<u32>(&m_error_cause, ERROR_CAUSE_BITS));
  mmio->Register(base | PI_ERROR_ADDRESS, MMIO::DirectRead<u32>(&m_error_address),
                 MMIO::InvalidWrite<u32>());

  // Reset code. On the GameCube, clearing bit 2 resets the disc drive.
  mmio->Register(base | PI_RESET_CODE, MMIO::ComplexRead<u32>([](Core::System& system, u32) {
                   const u32 code = system.GetProcessorInterface().m_reset_code;
                   DEBUG_LOG_FMT(PROCESSORINTERFACE, "PI_RESET_CODE -> {:08x}", code);
                   return code;
                 }),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   system.GetProcessorInterface().m_reset_code = val;
                   INFO_LOG_FMT(PROCESSORINTERFACE, "PI_RESET_CODE <- {:08x}", val);
                   if (!system.IsWii() && (val & 0x4) == 0)
                     system.GetDVDInterface().ResetDrive(true);
                 }));

  mmio->Register(base | PI_UNKNOWN, MMIO::DirectRead<u32>(&m_unknown),
                 MMIO::DirectWrite<u32>(&m_unknown, UNKNOWN_BITS));
  mmio->Register(base | PI_FLIPPER_REV, MMIO::Constant<u32>(FLIPPER_REVISION),
                 MMIO::InvalidWrite<u32>());
  mmio->Register(base | PI_FLIPPER_BUS_STRENGTH, MMIO::DirectRead<u32>(&m_flipper_bus_strength),
                 MMIO::DirectWrite<u32>(&m_flipper_bus_strength, BUS_STRENGTH_BITS));

  // Halfword reads come from the 32-bit registers; halfword writes are not
  // supported.
  for (u32 word = 0; word < 0x1000; word += 4)
  {
    mmio->Register(base | word, MMIO::ReadToLarger<u16>(mmio, base | word, 16),
                   MMIO::InvalidWrite<u16>());
    mmio->Register(base | (word + 2), MMIO::ReadToLarger<u16>(mmio, base | word, 0),
                   MMIO::InvalidWrite<u16>());
  }
}

void ProcessorInterfaceManager::UpdateException()
{
  auto& ppc_state = m_system.GetPPCState();
  if (m_interrupt_cause & m_interrupt_mask)
    ppc_state.Exceptions |= EXCEPTION_EXTERNAL_INT;
  else
    ppc_state.Exceptions &= ~EXCEPTION_EXTERNAL_INT;
}

void ProcessorInterfaceManager::SetInterrupt(u32 cause_mask, bool set)
{
  DEBUG_ASSERT_MSG(POWERPC, Core::IsCPUThread(), "SetInterrupt from wrong thread");

  const bool active = (m_interrupt_cause & cause_mask) != 0;
  if (set != active)
  {
    DEBUG_LOG_FMT(PROCESSORINTERFACE, "Interrupt {} {}", CauseName(cause_mask),
                  set ? "raised" : "cleared");
  }

  if (set)
    m_interrupt_cause |= cause_mask;
  else
    m_interrupt_cause &= ~cause_mask;
  UpdateException();
}

void ProcessorInterfaceManager::SetResetButton(bool pressed)
{
  // The cause bit holds the released state.
  SetInterrupt(INT_CAUSE_RST_BUTTON, !pressed);
}

void ProcessorInterfaceManager::ToggleResetButtonCallback(Core::System& system, u64 userdata,
                                                          s64)
{
  system.GetProcessorInterface().SetResetButton(userdata != 0);
}

void ProcessorInterfaceManager::IOSNotifyResetButtonCallback(Core::System& system, u64, s64)
{
  NotifyEventHook(system, false);
}

void ProcessorInterfaceManager::IOSNotifyPowerButtonCallback(Core::System& system, u64, s64)
{
  NotifyEventHook(system, true);
}

void ProcessorInterfaceManager::ResetButton_Tap()
{
  if (!Core::IsRunning(m_system))
    return;

  // Press now, tell IOS, release half a second later.
  auto& timing = m_system.GetCoreTiming();
  timing.ScheduleEvent(0, m_event_type_toggle_reset_button, true, CoreTiming::FromThread::ANY);
  timing.ScheduleEvent(0, m_event_type_ios_notify_reset_button, 0, CoreTiming::FromThread::ANY);
  timing.ScheduleEvent(m_system.GetSystemTimers().GetTicksPerSecond() / 2,
                       m_event_type_toggle_reset_button, false, CoreTiming::FromThread::ANY);
}

void ProcessorInterfaceManager::PowerButton_Tap()
{
  if (!Core::IsRunning(m_system))
    return;

  m_system.GetCoreTiming().ScheduleEvent(0, m_event_type_ios_notify_power_button, 0,
                                         CoreTiming::FromThread::ANY);
}

}  // namespace ProcessorInterface
