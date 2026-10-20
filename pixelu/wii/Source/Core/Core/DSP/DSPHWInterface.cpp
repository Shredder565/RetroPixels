// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The DSP's hardware registers at 0xff00-0xffff ("IFX"): the two mailboxes, the
// interrupt request line to the CPU, the DMA engine between main memory and the
// DSP memories, and the accelerator that streams samples out of ARAM.

#include "Core/DSP/DSPCore.h"

#include <atomic>
#include <cstdlib>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MemoryUtil.h"

#include "Core/DSP/DSPAccelerator.h"
#include "Core/DSP/DSPHost.h"
#include "Core/DSP/DSPTables.h"

namespace DSP
{
namespace
{
constexpr u32 MAIL_PENDING = 0x80000000;

// Replaces one 16-bit half of a 32-bit address register.
constexpr u32 WithHigh(u32 old_value, u16 high)
{
  return (u32{high} << 16) | (old_value & 0xffff);
}

constexpr u32 WithLow(u32 old_value, u16 low)
{
  return (old_value & 0xffff0000) | low;
}

const char* KnownRegisterName(u32 address)
{
  const u32 index = (address & 0xff) - 0xa0;
  if ((address & 0xff) < 0xa0 || index >= pdlabels.size())
    return nullptr;
  const pdlabel_t& label = pdlabels[index];
  return label.description != nullptr ? label.name : nullptr;
}
}  // namespace

std::atomic<u32>& SDSP::MailboxSlot(Mailbox mailbox)
{
  return m_mailbox[static_cast<size_t>(mailbox)];
}

const std::atomic<u32>& SDSP::MailboxSlot(Mailbox mailbox) const
{
  return m_mailbox[static_cast<size_t>(mailbox)];
}

void SDSP::InitializeIFX()
{
  m_ifx_regs.fill(0);
  MailboxSlot(Mailbox::CPU).store(0);
  MailboxSlot(Mailbox::DSP).store(0);
}

u32 SDSP::PeekMailbox(Mailbox mailbox) const
{
  return MailboxSlot(mailbox).load();
}

// Reading the low half consumes the mail: the pending flag drops.
u16 SDSP::ReadMailboxLow(Mailbox mailbox)
{
  std::atomic<u32>& slot = MailboxSlot(mailbox);
  const u32 mail = slot.load(std::memory_order_acquire);
  slot.store(mail & ~MAIL_PENDING, std::memory_order_release);

#if defined(_DEBUG) || defined(DEBUGFAST)
  DEBUG_LOG_FMT(DSP_MAIL, "{} mailbox read low, now {:#010x} (pc={:#06x})",
                mailbox == Mailbox::DSP ? "DSP" : "CPU", PeekMailbox(mailbox), pc);
#endif

  return static_cast<u16>(mail & 0xffff);
}

u16 SDSP::ReadMailboxHigh(Mailbox mailbox)
{
  // The pending flag is part of the value read here.
  return static_cast<u16>(PeekMailbox(mailbox) >> 16);
}

// Writing the low half completes a mail and raises the pending flag.
void SDSP::WriteMailboxLow(Mailbox mailbox, u16 value)
{
  std::atomic<u32>& slot = MailboxSlot(mailbox);
  const u32 mail = WithLow(slot.load(std::memory_order_acquire), value);
  slot.store(mail | MAIL_PENDING, std::memory_order_release);

#if defined(_DEBUG) || defined(DEBUGFAST)
  DEBUG_LOG_FMT(DSP_MAIL, "{} mailbox write low, now {:#010x} (pc={:#06x})",
                mailbox == Mailbox::DSP ? "DSP" : "CPU", PeekMailbox(mailbox), pc);
#endif
}

// Writing the high half starts a new mail, so the pending flag is cleared.
void SDSP::WriteMailboxHigh(Mailbox mailbox, u16 value)
{
  std::atomic<u32>& slot = MailboxSlot(mailbox);
  const u32 mail = WithHigh(slot.load(std::memory_order_acquire), value);
  slot.store(mail & ~MAIL_PENDING, std::memory_order_release);
}

void SDSP::WriteIFX(u32 address, u16 value)
{
  m_dsp_core.LogIFXWrite(static_cast<u16>(address), value);

  const u32 reg = address & 0xff;
  Accelerator& accel = *m_accelerator;

  switch (reg)
  {
  case DSP_DIRQ:
    if ((value & 1) != 0)
      Host::InterruptRequest();
    else if (value != 0)
      WARN_LOG_FMT(DSPLLE, "DIRQ written with {:#06x} at pc={:#06x}", value, pc);
    // libasnd writes 0 here to "clear" the interrupt; that is harmless.
    break;

  case DSP_DMBH:
    WriteMailboxHigh(Mailbox::DSP, value);
    break;
  case DSP_DMBL:
    WriteMailboxLow(Mailbox::DSP, value);
    break;
  case DSP_CMBH:
    WriteMailboxHigh(Mailbox::CPU, value);
    break;
  case DSP_CMBL:
    WriteMailboxLow(Mailbox::CPU, value);
    break;

  case DSP_DSBL:
    // The length write starts the transfer, which we finish immediately. The busy
    // bit in DSCR is only up for the duration of this call.
    m_ifx_regs[DSP_DSBL] = value;
    m_ifx_regs[DSP_DSCR] |= 4;
    if (m_ifx_regs[DSP_AMDM] == 0)
      RunDMA();
    else
      NOTICE_LOG_FMT(DSPLLE, "DMA ignored: AMDM masks it");
    m_ifx_regs[DSP_DSCR] &= static_cast<u16>(~4u);
    m_ifx_regs[DSP_DSBL] = 0;
    break;

  case DSP_DSPA:
  case DSP_DSMAH:
  case DSP_DSMAL:
  case DSP_DSCR:
    m_ifx_regs[reg] = value;
    break;

  case DSP_ACSAH:
    accel.SetStartAddress(WithHigh(accel.GetStartAddress(), value));
    break;
  case DSP_ACSAL:
    accel.SetStartAddress(WithLow(accel.GetStartAddress(), value));
    break;
  case DSP_ACEAH:
    accel.SetEndAddress(WithHigh(accel.GetEndAddress(), value));
    break;
  case DSP_ACEAL:
    accel.SetEndAddress(WithLow(accel.GetEndAddress(), value));
    break;
  case DSP_ACCAH:
    accel.SetCurrentAddress(WithHigh(accel.GetCurrentAddress(), value));
    break;
  case DSP_ACCAL:
    accel.SetCurrentAddress(WithLow(accel.GetCurrentAddress(), value));
    break;
  case DSP_FORMAT:
    accel.SetSampleFormat(value);
    break;
  case DSP_GAIN:
    accel.SetGain(static_cast<s16>(value));
    break;
  case DSP_YN1:
    accel.SetYn1(static_cast<s16>(value));
    break;
  case DSP_YN2:
    accel.SetYn2(static_cast<s16>(value));
    break;
  case DSP_PRED_SCALE:
    accel.SetPredScale(value);
    break;
  case DSP_ACDRAW:
    accel.WriteRaw(value);
    break;
  case DSP_ACIN:
    accel.SetInput(value);
    break;

  default:
    if (const char* name = KnownRegisterName(address))
      DEBUG_LOG_FMT(DSPLLE, "{:04x} IFX write {} = {:04x}", pc, name, value);
    else
      ERROR_LOG_FMT(DSPLLE, "{:04x} IFX write to unhandled {:04x} = {:04x}", pc, address, value);
    m_ifx_regs[reg] = value;
    break;
  }
}

u16 SDSP::ReadIFXRegister(u16 address)
{
  const u32 reg = address & 0xff;
  Accelerator& accel = *m_accelerator;

  switch (reg)
  {
  case DSP_DMBH:
    return ReadMailboxHigh(Mailbox::DSP);
  case DSP_DMBL:
    return ReadMailboxLow(Mailbox::DSP);
  case DSP_CMBH:
    return ReadMailboxHigh(Mailbox::CPU);
  case DSP_CMBL:
    return ReadMailboxLow(Mailbox::CPU);

  case DSP_DSCR:
    return m_ifx_regs[reg];

  case DSP_ACSAH:
    return static_cast<u16>(accel.GetStartAddress() >> 16);
  case DSP_ACSAL:
    return static_cast<u16>(accel.GetStartAddress() & 0xffff);
  case DSP_ACEAH:
    return static_cast<u16>(accel.GetEndAddress() >> 16);
  case DSP_ACEAL:
    return static_cast<u16>(accel.GetEndAddress() & 0xffff);
  case DSP_ACCAH:
    return static_cast<u16>(accel.GetCurrentAddress() >> 16);
  case DSP_ACCAL:
    return static_cast<u16>(accel.GetCurrentAddress() & 0xffff);
  case DSP_FORMAT:
    return accel.GetSampleFormat();
  case DSP_GAIN:
    return static_cast<u16>(accel.GetGain());
  case DSP_YN1:
    return static_cast<u16>(accel.GetYn1());
  case DSP_YN2:
    return static_cast<u16>(accel.GetYn2());
  case DSP_PRED_SCALE:
    return accel.GetPredScale();
  case DSP_ACDSAMP:
    // The ADPCM coefficients live in the 16 registers starting at 0xffa0.
    return accel.ReadSample(reinterpret_cast<const s16*>(&m_ifx_regs[DSP_COEF_A1_0]));
  case DSP_ACDRAW:
    return accel.ReadRaw();
  case DSP_ACIN:
    return accel.GetInput();

  default:
  {
    const u16 stored = m_ifx_regs[reg];
    if (const char* name = KnownRegisterName(address))
      DEBUG_LOG_FMT(DSPLLE, "{:04x} IFX read {} = {:04x}", pc, name, stored);
    else
      ERROR_LOG_FMT(DSPLLE, "{:04x} IFX read from unhandled {:04x} = {:04x}", pc, address, stored);
    return stored;
  }
  }
}

u16 SDSP::ReadIFX(u16 address)
{
  const u16 value = ReadIFXRegister(address);
  m_dsp_core.LogIFXRead(address, value);
  return value;
}

// The DMA helpers take the DSP side address as a byte offset (DSPA * 2) and
// return the DSP memory that took part, for the capture log.

const u8* SDSP::CopyToIRAM(u16 byte_offset, u32 addr, u32 size)
{
  Common::UnWriteProtectMemory(iram, DSP_IRAM_BYTE_SIZE, false);
  Host::DMAToDSP(iram + byte_offset / 2, addr, size);
  Common::WriteProtectMemory(iram, DSP_IRAM_BYTE_SIZE, false);

  Host::CodeLoaded(m_dsp_core, addr, size);
  NOTICE_LOG_FMT(DSPLLE, "Ucode DMA from {:#010x} to IRAM {:#06x} (crc {:#08x})", addr,
                 byte_offset, m_iram_crc);

  return reinterpret_cast<const u8*>(iram) + byte_offset;
}

const u8* SDSP::CopyFromIRAM(u16 byte_offset, u32 addr, u32 size)
{
  // Not supported by the hardware as far as anyone knows.
  ERROR_LOG_FMT(DSPLLE, "IRAM -> RAM DMA ignored: IRAM {:#06x} to {:#010x}, {:#x} bytes",
                byte_offset / 2, addr, size);
  return nullptr;
}

const u8* SDSP::CopyToDRAM(u16 byte_offset, u32 addr, u32 size)
{
  Host::DMAToDSP(dram + byte_offset / 2, addr, size);
  DEBUG_LOG_FMT(DSPLLE, "DMA RAM {:#010x} -> DRAM {:#06x}, {:#x} bytes", addr, byte_offset / 2,
                size);
  return reinterpret_cast<const u8*>(dram) + byte_offset;
}

const u8* SDSP::CopyFromDRAM(u16 byte_offset, u32 addr, u32 size)
{
  Host::DMAFromDSP(dram + byte_offset / 2, addr, size);
  DEBUG_LOG_FMT(DSPLLE, "DMA DRAM {:#06x} -> RAM {:#010x}, {:#x} bytes", byte_offset / 2, addr,
                size);
  return reinterpret_cast<const u8*>(dram) + byte_offset;
}

void SDSP::RunDMA()
{
  const u32 main_addr = (u32{m_ifx_regs[DSP_DSMAH]} << 16) | m_ifx_regs[DSP_DSMAL];
  const u16 control = m_ifx_regs[DSP_DSCR];
  const u16 byte_offset = static_cast<u16>(m_ifx_regs[DSP_DSPA] * 2);
  const u16 length = m_ifx_regs[DSP_DSBL];

  if (length > 0x4000)
  {
    // No ucode does this; it can only mean the emulated program went wrong.
    ERROR_LOG_FMT(DSPLLE,
                  "Bad DMA length: pc {:04x}, control {:04x}, address {:08x}, DSP address "
                  "{:04x}, length {:04x}",
                  pc, control, main_addr, byte_offset, length);
    std::exit(0);
  }

#if defined(_DEBUG) || defined(DEBUGFAST)
  DEBUG_LOG_FMT(DSPLLE, "DMA: pc {:04x}, control {:04x}, address {:08x}, DSP address {:04x}, "
                        "length {:04x}",
                pc, control, main_addr, byte_offset, length);
#endif

  const bool to_cpu = (control & DSP_CR_TO_CPU) != 0;
  const bool instruction_memory = (control & DSP_CR_IMEM) != 0;

  const u8* touched;
  if (instruction_memory)
  {
    touched = to_cpu ? CopyFromIRAM(byte_offset, main_addr, length) :
                       CopyToIRAM(byte_offset, main_addr, length);
  }
  else
  {
    touched = to_cpu ? CopyFromDRAM(byte_offset, main_addr, length) :
                       CopyToDRAM(byte_offset, main_addr, length);
  }

  if (touched != nullptr)
    m_dsp_core.LogDMA(control, main_addr, byte_offset, length, touched);
}
}  // namespace DSP
