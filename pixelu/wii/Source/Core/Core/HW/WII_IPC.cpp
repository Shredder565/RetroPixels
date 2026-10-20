// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WII_IPC.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/IOS/IOS.h"
#include "Core/System.h"

// IPC protocol, as IOS uses it. Broadway writes the address of a 64-byte
// request to PPCMSG and sets X1 ("execute"). Starlet answers by putting a
// reply address in ARMMSG and setting Y1 ("reply ready"), and acknowledges
// requests with Y2. X2 means "IOS reloaded". IY1/IY2 enable Broadway's
// interrupt for Y1/Y2, IX1/IX2 Starlet's for X1/X2.

namespace IOS
{
namespace
{
// Register offsets.
enum : u32
{
  IPC_PPCMSG = 0x00,
  IPC_PPCCTRL = 0x04,
  IPC_ARMMSG = 0x08,
  IPC_ARMCTRL = 0x0c,
  PPCSPEED = 0x18,
  VISOLID = 0x24,
  PPC_IRQFLAG = 0x30,
  PPC_IRQMASK = 0x34,
  ARM_IRQFLAG = 0x38,
  ARM_IRQMASK = 0x3c,
  AHBPROT = 0x64,
  GPIOB_OUT = 0xc0,
  GPIOB_DIR = 0xc4,
  GPIOB_IN = 0xc8,
  GPIO_OUT = 0xe0,
  GPIO_DIR = 0xe4,
  GPIO_IN = 0xe8,
  UNK_180 = 0x180,
  HW_RESETS = 0x194,
  UNK_1CC = 0x1cc,
  UNK_1D0 = 0x1d0,
};

// Control flag bits (savestate layout).
constexpr u8 X1 = 0x01;
constexpr u8 X2 = 0x02;
constexpr u8 Y1 = 0x04;
constexpr u8 Y2 = 0x08;
constexpr u8 IX1 = 0x10;
constexpr u8 IX2 = 0x20;
constexpr u8 IY1 = 0x40;
constexpr u8 IY2 = 0x80;

// Bits of the PPCCTRL register.
constexpr u32 PPCCTRL_X1 = 0x01;
constexpr u32 PPCCTRL_Y2 = 0x02;  // write 1 to clear
constexpr u32 PPCCTRL_Y1 = 0x04;  // write 1 to clear
constexpr u32 PPCCTRL_X2 = 0x08;
constexpr u32 PPCCTRL_IY1 = 0x10;
constexpr u32 PPCCTRL_IY2 = 0x20;

// GPIO pins that Broadway may drive (through the GPIOB registers).
constexpr Common::Flags<GPIO> BROADWAY_PINS = {GPIO::SLOT_LED, GPIO::SLOT_IN,  GPIO::SENSOR_BAR,
                                               GPIO::DO_EJECT, GPIO::AVE_SCL, GPIO::AVE_SDA};

constexpr u32 RESET_DI = 0x400;

bool Has(u8 flags, u8 bit)
{
  return (flags & bit) != 0;
}
void Assign(u8& flags, u8 bit, bool on)
{
  flags = on ? (flags | bit) : (flags & ~bit);
}

u32 ReadGPIOInputs(Core::System& system)
{
  Common::Flags<GPIO> inputs;
  inputs[GPIO::SLOT_IN] = system.GetDVDInterface().IsDiscInside();
  return inputs.m_hex;
}

void EjectIfRequested(Core::System& system, Common::Flags<GPIO> out)
{
  // SENSOR_BAR is read by the emulated Wii remote camera; AVE and the slot
  // LED have no effect.
  if (!out[GPIO::DO_EJECT])
    return;
  INFO_LOG_FMT(WII_IPC, "Disc eject requested through GPIO");
  system.GetDVDInterface().EjectDisc(Core::CPUThreadGuard{system}, DVD::EjectCause::Software);
}
// Saves or loads each field in turn.
template <typename... Fields>
void DoFields(PointerWrap& p, Fields&... fields)
{
  (p.Do(fields), ...);
}
}  // namespace

WiiIPC::WiiIPC(Core::System& system) : m_system(system)
{
}

WiiIPC::~WiiIPC() = default;

void WiiIPC::DoState(PointerWrap& p)
{
  DoFields(p, m_ppc_msg, m_arm_msg, m_ctrl);
  DoFields(p, m_ppc_irq_flags, m_ppc_irq_masks, m_arm_irq_flags, m_arm_irq_masks);
  DoFields(p, m_gpio_dir, m_gpio_out, m_resets);
}

void WiiIPC::InitState()
{
  m_ctrl = 0;
  m_ppc_msg = m_arm_msg = 0;
  m_ppc_irq_flags = m_arm_irq_flags = m_arm_irq_masks = 0;
  m_ppc_irq_masks = INT_CAUSE_IPC_BROADWAY;

  // Every pin is an output except the inputs EJECT_BTN, SLOT_IN and EEP_MISO.
  m_gpio_dir = {GPIO::POWER,    GPIO::SHUTDOWN, GPIO::FAN,      GPIO::DC_DC,    GPIO::DI_SPIN,
                GPIO::SLOT_LED, GPIO::SENSOR_BAR, GPIO::DO_EJECT, GPIO::EEP_CS, GPIO::EEP_CLK,
                GPIO::EEP_MOSI, GPIO::AVE_SCL,  GPIO::AVE_SDA,  GPIO::DEBUG0,   GPIO::DEBUG1,
                GPIO::DEBUG2,   GPIO::DEBUG3,   GPIO::DEBUG4,   GPIO::DEBUG5,   GPIO::DEBUG6,
                GPIO::DEBUG7};
  m_gpio_out = {};

  // A clear bit holds a device in reset; start with everything running.
  m_resets = 0xffffffff;
}

void WiiIPC::Init()
{
  InitState();
  m_event_type_update_interrupts =
      m_system.GetCoreTiming().RegisterEvent("IPCInterrupt", UpdateInterruptsCallback);
}

void WiiIPC::Reset()
{
  INFO_LOG_FMT(WII_IPC, "Reset");
  InitState();
}

void WiiIPC::Shutdown()
{
}

// --- IPC control -----------------------------------------------------------------------

u8 WiiIPC::ReadPPCControl() const
{
  u8 value = 0;
  value |= Has(m_ctrl, X1) ? PPCCTRL_X1 : 0;
  value |= Has(m_ctrl, Y2) ? PPCCTRL_Y2 : 0;
  value |= Has(m_ctrl, Y1) ? PPCCTRL_Y1 : 0;
  value |= Has(m_ctrl, X2) ? PPCCTRL_X2 : 0;
  value |= Has(m_ctrl, IY1) ? PPCCTRL_IY1 : 0;
  value |= Has(m_ctrl, IY2) ? PPCCTRL_IY2 : 0;
  return value;
}

void WiiIPC::WritePPCControl(u32 value)
{
  Assign(m_ctrl, X1, value & PPCCTRL_X1);
  Assign(m_ctrl, X2, value & PPCCTRL_X2);
  if (value & PPCCTRL_Y1)
    Assign(m_ctrl, Y1, false);
  if (value & PPCCTRL_Y2)
    Assign(m_ctrl, Y2, false);
  Assign(m_ctrl, IY1, value & PPCCTRL_IY1);
  Assign(m_ctrl, IY2, value & PPCCTRL_IY2);

  // Acknowledging Y1/Y2 with its interrupt enabled raises the interrupt,
  // even though the bit is cleared.
  if (((value & PPCCTRL_Y1) && Has(m_ctrl, IY1)) || ((value & PPCCTRL_Y2) && Has(m_ctrl, IY2)))
    m_ppc_irq_flags |= INT_CAUSE_IPC_BROADWAY;

  auto* ios = m_system.GetIOS();
  if (Has(m_ctrl, X1))
    ios->EnqueueIPCRequest(m_ppc_msg);
  ios->UpdateIPC();
  m_system.GetCoreTiming().ScheduleEvent(0, m_event_type_update_interrupts, 0);
}

void WiiIPC::RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  // Changes of the flag or mask registers: let IOS react and re-evaluate the
  // interrupt line.
  const auto notify = [](Core::System& system) {
    system.GetIOS()->UpdateIPC();
    system.GetCoreTiming().ScheduleEvent(0, system.GetWiiIPC().m_event_type_update_interrupts, 0);
  };

  mmio->Register(base | IPC_PPCMSG, MMIO::InvalidRead<u32>(), MMIO::DirectWrite<u32>(&m_ppc_msg));
  mmio->Register(base | IPC_PPCCTRL, MMIO::ComplexRead<u32>([](Core::System& system, u32) {
                   return static_cast<u32>(system.GetWiiIPC().ReadPPCControl());
                 }),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   system.GetWiiIPC().WritePPCControl(val);
                 }));
  mmio->Register(base | IPC_ARMMSG, MMIO::DirectRead<u32>(&m_arm_msg), MMIO::InvalidWrite<u32>());

  mmio->Register(base | PPC_IRQFLAG, MMIO::InvalidRead<u32>(),
                 MMIO::ComplexWrite<u32>([notify](Core::System& system, u32, u32 val) {
                   system.GetWiiIPC().m_ppc_irq_flags &= ~val;
                   notify(system);
                 }));
  mmio->Register(base | PPC_IRQMASK, MMIO::InvalidRead<u32>(),
                 MMIO::ComplexWrite<u32>([notify](Core::System& system, u32, u32 val) {
                   auto& ipc = system.GetWiiIPC();
                   ipc.m_ppc_irq_masks = val;
                   // IOS sets this mask bit while booting; treat it as a reset.
                   if (val & INT_CAUSE_IPC_BROADWAY)
                     ipc.Reset();
                   notify(system);
                 }));

  // No access restrictions are emulated.
  mmio->Register(base | AHBPROT, MMIO::Constant<u32>(0xFFFFFFFF), MMIO::InvalidWrite<u32>());

  // GPIOs. There is one set of pin states; the Broadway registers (GPIOB_*)
  // may change only Broadway's pins and the Starlet registers only the
  // others. Both sets read all pins. Some homebrew uses the Starlet set.
  mmio->Register(base | GPIOB_OUT, MMIO::DirectRead<u32>(&m_gpio_out.m_hex),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& ipc = system.GetWiiIPC();
                   const u32 pins = BROADWAY_PINS.m_hex;
                   ipc.m_gpio_out.m_hex = (ipc.m_gpio_out.m_hex & ~pins) | (val & pins);
                   EjectIfRequested(system, ipc.m_gpio_out);
                 }));
  mmio->Register(base | GPIOB_DIR, MMIO::DirectRead<u32>(&m_gpio_dir.m_hex),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& ipc = system.GetWiiIPC();
                   const u32 pins = BROADWAY_PINS.m_hex;
                   ipc.m_gpio_dir.m_hex = (ipc.m_gpio_dir.m_hex & ~pins) | (val & pins);
                 }));
  mmio->Register(base | GPIOB_IN, MMIO::ComplexRead<u32>([](Core::System& system, u32) {
                   return ReadGPIOInputs(system);
                 }),
                 MMIO::Nop<u32>());

  mmio->Register(base | GPIO_OUT, MMIO::DirectRead<u32>(&m_gpio_out.m_hex),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& ipc = system.GetWiiIPC();
                   const u32 pins = BROADWAY_PINS.m_hex;
                   ipc.m_gpio_out.m_hex = (ipc.m_gpio_out.m_hex & pins) | (val & ~pins);
                   EjectIfRequested(system, ipc.m_gpio_out);
                 }));
  mmio->Register(base | GPIO_DIR, MMIO::DirectRead<u32>(&m_gpio_dir.m_hex),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& ipc = system.GetWiiIPC();
                   const u32 pins = BROADWAY_PINS.m_hex;
                   ipc.m_gpio_dir.m_hex = (ipc.m_gpio_dir.m_hex & pins) | (val & ~pins);
                 }));
  mmio->Register(base | GPIO_IN, MMIO::ComplexRead<u32>([](Core::System& system, u32) {
                   return ReadGPIOInputs(system);
                 }),
                 MMIO::Nop<u32>());

  // Device resets: clearing the DI bit resets the drive; the DI_SPIN GPIO
  // prevents it from spinning up.
  mmio->Register(base | HW_RESETS, MMIO::DirectRead<u32>(&m_resets),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& ipc = system.GetWiiIPC();
                   const bool reset_di = (ipc.m_resets & RESET_DI) != 0 && (val & RESET_DI) == 0;
                   ipc.m_resets = val;
                   if (!reset_di)
                     return;
                   const bool spinup = !ipc.m_gpio_out[GPIO::DI_SPIN];
                   INFO_LOG_FMT(WII_IPC, "DI reset ({} spin-up)", spinup ? "with" : "without");
                   system.GetDVDInterface().ResetDrive(spinup);
                 }));

  // Registers games touch that need no emulation.
  mmio->Register(base | PPCSPEED, MMIO::InvalidRead<u32>(), MMIO::Nop<u32>());
  mmio->Register(base | VISOLID, MMIO::InvalidRead<u32>(), MMIO::Nop<u32>());
  for (u32 offset : {UNK_180, UNK_1CC, UNK_1D0})
    mmio->Register(base | offset, MMIO::Constant<u32>(0), MMIO::Nop<u32>());
}

// --- Interrupts -------------------------------------------------------------------------

void WiiIPC::UpdateInterruptsCallback(Core::System& system, u64, s64)
{
  system.GetWiiIPC().UpdateInterrupts();
}

void WiiIPC::UpdateInterrupts()
{
  if ((Has(m_ctrl, Y1) && Has(m_ctrl, IY1)) || (Has(m_ctrl, Y2) && Has(m_ctrl, IY2)))
    m_ppc_irq_flags |= INT_CAUSE_IPC_BROADWAY;
  if ((Has(m_ctrl, X1) && Has(m_ctrl, IX1)) || (Has(m_ctrl, X2) && Has(m_ctrl, IX2)))
    m_ppc_irq_flags |= INT_CAUSE_IPC_STARLET;

  m_system.GetProcessorInterface().SetInterrupt(ProcessorInterface::INT_CAUSE_WII_IPC,
                                                (m_ppc_irq_flags & m_ppc_irq_masks) != 0);
}

void WiiIPC::ClearX1()
{
  Assign(m_ctrl, X1, false);
}

// The interrupt follows about 100 time base ticks after Y1/Y2 is set
// (hardware measurement).
void WiiIPC::GenerateAck(u32 address)
{
  Assign(m_ctrl, Y2, true);
  DEBUG_LOG_FMT(WII_IPC, "Ack {:08x} for request {:08x}", address, m_ppc_msg);
  m_system.GetCoreTiming().ScheduleEvent(100_tbticks, m_event_type_update_interrupts);
}

void WiiIPC::GenerateReply(u32 address)
{
  m_arm_msg = address;
  Assign(m_ctrl, Y1, true);
  DEBUG_LOG_FMT(WII_IPC, "Reply {:08x} for request {:08x}", address, m_ppc_msg);
  m_system.GetCoreTiming().ScheduleEvent(100_tbticks, m_event_type_update_interrupts);
}

bool WiiIPC::IsReady() const
{
  return !Has(m_ctrl, Y1) && !Has(m_ctrl, Y2) && (m_ppc_irq_flags & INT_CAUSE_IPC_BROADWAY) == 0;
}
}  // namespace IOS
