// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

class PointerWrap;

namespace Core
{
class System;
}
namespace CoreTiming
{
struct EventType;
}
namespace MMIO
{
class Mapping;
}

// The processor interface (PI, 0x0C003000): the interrupt controller in front
// of the CPU's external interrupt line, the CPU-side FIFO pointers used by the
// write gatherer, the reset code and a few board registers.
namespace ProcessorInterface
{
// Interrupt cause / mask bits.
enum InterruptCause
{
  INT_CAUSE_PI = 0x1,           // GP runtime error
  INT_CAUSE_RSW = 0x2,          // Reset switch
  INT_CAUSE_DI = 0x4,           // DVD interface
  INT_CAUSE_SI = 0x8,           // Serial interface
  INT_CAUSE_EXI = 0x10,         // Expansion interface
  INT_CAUSE_AI = 0x20,          // Audio interface (streaming)
  INT_CAUSE_DSP = 0x40,         // DSP interface
  INT_CAUSE_MEMORY = 0x80,      // Memory interface
  INT_CAUSE_VI = 0x100,         // Video interface
  INT_CAUSE_PE_TOKEN = 0x200,   // Pixel engine token
  INT_CAUSE_PE_FINISH = 0x400,  // Pixel engine finish
  INT_CAUSE_CP = 0x800,         // Command processor FIFO
  INT_CAUSE_DEBUG = 0x1000,     // Devkit debugger
  INT_CAUSE_HSP = 0x2000,       // High speed port
  INT_CAUSE_WII_IPC = 0x4000,   // Wii IPC
  // Level of the reset button (1 = released). A state bit, not an interrupt.
  INT_CAUSE_RST_BUTTON = 0x10000
};

// Register offsets.
enum
{
  PI_INTERRUPT_CAUSE = 0x00,
  PI_INTERRUPT_MASK = 0x04,
  PI_FIFO_BASE = 0x0C,
  PI_FIFO_END = 0x10,
  PI_FIFO_WPTR = 0x14,
  PI_FIFO_RESET = 0x18,
  PI_ERROR_CAUSE = 0x1C,
  PI_ERROR_ADDRESS = 0x20,
  PI_RESET_CODE = 0x24,
  PI_UNKNOWN = 0x28,
  PI_FLIPPER_REV = 0x2C,
  // Bus drive strength; written by the boot ROM.
  PI_FLIPPER_BUS_STRENGTH = 0x30
};

enum ErrorCause : u32
{
  NoError = 0,
  MisalignedAddress = 1,
  IncorrectTransferType = 2,
  UnsupportedTransferSize = 3,
  AddressOutOfRange = 4,
  WriteToROM = 5,
  ReadFromGXFIFO = 6,
  Reserved = 7,
};

class ProcessorInterfaceManager
{
public:
  explicit ProcessorInterfaceManager(Core::System& system);
  ProcessorInterfaceManager(const ProcessorInterfaceManager& other) = delete;
  ProcessorInterfaceManager(ProcessorInterfaceManager&& other) = delete;
  ProcessorInterfaceManager& operator=(const ProcessorInterfaceManager& other) = delete;
  ProcessorInterfaceManager& operator=(ProcessorInterfaceManager&& other) = delete;
  ~ProcessorInterfaceManager();

  void Init();
  void DoState(PointerWrap& p);

  void RegisterMMIO(MMIO::Mapping* mmio, u32 base);

  u32 GetMask() const { return m_interrupt_mask; }
  u32 GetCause() const { return m_interrupt_cause; }

  // Raises (or lowers) the given cause bits and updates the CPU's external
  // interrupt line. CPU thread only.
  void SetInterrupt(u32 cause_mask, bool set = true);

  // Press-and-release of the console buttons; may be called from any thread.
  void ResetButton_Tap();
  void PowerButton_Tap();

  u32 m_interrupt_cause = 0;
  u32 m_interrupt_mask = 0;

  // CPU-side FIFO window (the write gatherer's destination).
  u32 m_fifo_cpu_base = 0;
  u32 m_fifo_cpu_end = 0;
  u32 m_fifo_cpu_write_pointer = 0;

  u32 m_error_cause = 0;
  u32 m_error_address = 0;
  u32 m_unknown = 0x000001FF;
  u32 m_flipper_bus_strength = 0x02492492;

private:
  void UpdateException();
  void SetResetButton(bool set);

  static void ToggleResetButtonCallback(Core::System& system, u64 userdata, s64 cyclesLate);
  static void IOSNotifyResetButtonCallback(Core::System& system, u64 userdata, s64 cyclesLate);
  static void IOSNotifyPowerButtonCallback(Core::System& system, u64 userdata, s64 cyclesLate);

  CoreTiming::EventType* m_event_type_toggle_reset_button = nullptr;
  CoreTiming::EventType* m_event_type_ios_notify_reset_button = nullptr;
  CoreTiming::EventType* m_event_type_ios_notify_power_button = nullptr;

  u32 m_reset_code = 0;

  Core::System& m_system;
};
}  // namespace ProcessorInterface
