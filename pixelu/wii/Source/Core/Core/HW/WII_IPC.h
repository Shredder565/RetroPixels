// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/BitUtils.h"
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

// Hollywood registers at 0x0D000000 as seen from Broadway: the IPC mailbox
// to Starlet (where IOS runs; emulated at a high level), the interrupt
// flags/masks, GPIOs and device resets.
namespace IOS
{
enum StarletInterruptCause
{
  INT_CAUSE_TIMER = 0x1,
  INT_CAUSE_NAND = 0x2,
  INT_CAUSE_AES = 0x4,
  INT_CAUSE_SHA1 = 0x8,
  INT_CAUSE_EHCI = 0x10,
  INT_CAUSE_OHCI0 = 0x20,
  INT_CAUSE_OHCI1 = 0x40,
  INT_CAUSE_SD = 0x80,
  INT_CAUSE_WIFI = 0x100,

  INT_CAUSE_GPIO_BROADWAY = 0x400,
  INT_CAUSE_GPIO_STARLET = 0x800,

  INT_CAUSE_RST_BUTTON = 0x40000,

  INT_CAUSE_IPC_BROADWAY = 0x40000000,
  INT_CAUSE_IPC_STARLET = 0x80000000
};

enum class GPIO : u32
{
  POWER = 0x1,
  SHUTDOWN = 0x2,
  FAN = 0x4,
  DC_DC = 0x8,
  DI_SPIN = 0x10,
  SLOT_LED = 0x20,
  EJECT_BTN = 0x40,
  SLOT_IN = 0x80,
  SENSOR_BAR = 0x100,
  DO_EJECT = 0x200,
  EEP_CS = 0x400,
  EEP_CLK = 0x800,
  EEP_MOSI = 0x1000,
  EEP_MISO = 0x2000,
  AVE_SCL = 0x4000,
  AVE_SDA = 0x8000,
  DEBUG0 = 0x10000,
  DEBUG1 = 0x20000,
  DEBUG2 = 0x40000,
  DEBUG3 = 0x80000,
  DEBUG4 = 0x100000,
  DEBUG5 = 0x200000,
  DEBUG6 = 0x400000,
  DEBUG7 = 0x800000,
};

class WiiIPC
{
public:
  explicit WiiIPC(Core::System& system);
  WiiIPC(const WiiIPC&) = delete;
  WiiIPC(WiiIPC&&) = delete;
  WiiIPC& operator=(const WiiIPC&) = delete;
  WiiIPC& operator=(WiiIPC&&) = delete;
  ~WiiIPC();

  void Init();
  void Reset();
  void Shutdown();
  void DoState(PointerWrap& p);

  void RegisterMMIO(MMIO::Mapping* mmio, u32 base);

  // Called by IOS HLE.
  void ClearX1();
  void GenerateAck(u32 address);
  void GenerateReply(u32 address);

  // No reply or acknowledgement pending.
  bool IsReady() const;

  Common::Flags<GPIO> GetGPIOOutFlags() const { return m_gpio_out; }

private:
  void InitState();

  static void UpdateInterruptsCallback(Core::System& system, u64 userdata, s64 cycles_late);
  void UpdateInterrupts();

  u8 ReadPPCControl() const;
  void WritePPCControl(u32 value);

  u32 m_ppc_msg = 0;
  u32 m_arm_msg = 0;
  // IPC control flags, one bit each (see the .cpp).
  u8 m_ctrl = 0;

  u32 m_ppc_irq_flags = 0;
  u32 m_ppc_irq_masks = 0;
  u32 m_arm_irq_flags = 0;
  u32 m_arm_irq_masks = 0;

  Common::Flags<GPIO> m_gpio_dir{};
  Common::Flags<GPIO> m_gpio_out{};

  u32 m_resets = 0;

  CoreTiming::EventType* m_event_type_update_interrupts = nullptr;

  Core::System& m_system;
};
}  // namespace IOS
