// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/EXI_Channel.h"

#include <memory>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

#include "Core/CoreTiming.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/MMIO.h"
#include "Core/Movie.h"
#include "Core/System.h"

namespace ExpansionInterface
{
namespace
{
// Register offsets within a channel's block.
constexpr u32 REG_PARAMETER = 0x00;
constexpr u32 REG_DMA_ADDRESS = 0x04;
constexpr u32 REG_DMA_LENGTH = 0x08;
constexpr u32 REG_CONTROL = 0x0C;
constexpr u32 REG_IMMEDIATE = 0x10;

// Parameter register bits.
constexpr u32 PARAM_EXI_MASK = 1u << 0;
constexpr u32 PARAM_EXI_IRQ = 1u << 1;
constexpr u32 PARAM_TC_MASK = 1u << 2;
constexpr u32 PARAM_TC_IRQ = 1u << 3;
constexpr u32 PARAM_CLOCK = 7u << 4;
constexpr u32 PARAM_CS_SHIFT = 7;
constexpr u32 PARAM_CS = 7u << PARAM_CS_SHIFT;
constexpr u32 PARAM_EXT_MASK = 1u << 10;
constexpr u32 PARAM_EXT_IRQ = 1u << 11;
constexpr u32 PARAM_EXT_PRESENT = 1u << 12;
constexpr u32 PARAM_ROM_DISABLE = 1u << 13;

// Control register fields.
constexpr u32 CTRL_START = 1u << 0;
constexpr u32 CTRL_USE_DMA = 1u << 1;

enum class Direction : u32
{
  Read = 0,
  Write = 1,
  ReadWrite = 2,
};

constexpr Direction ControlDirection(u32 control)
{
  return static_cast<Direction>((control >> 2) & 3);
}

constexpr u32 ControlImmediateLength(u32 control)
{
  return ((control >> 4) & 3) + 1;
}

constexpr u8 ChipSelectOf(u32 param)
{
  return static_cast<u8>((param & PARAM_CS) >> PARAM_CS_SHIFT);
}

constexpr bool AllSet(u32 value, u32 bits)
{
  return (value & bits) == bits;
}
}  // namespace

CEXIChannel::CEXIChannel(Core::System& system, u32 channel_id,
                         const Memcard::HeaderData& memcard_header_data)
    : m_system(system), m_channel_id(channel_id), m_memcard_header_data(memcard_header_data)
{
  // The memory card channels power up reporting an insertion event; channel 1 starts with
  // chip select 0 (its memory card) asserted.
  if (HasExternalInterruptLine())
    m_param |= PARAM_EXT_IRQ;
  if (m_channel_id == 1)
    m_param |= u32{1} << PARAM_CS_SHIFT;

  for (std::unique_ptr<IEXIDevice>& slot : m_devices)
    slot = EXIDevice_Create(system, EXIDeviceType::None, ChannelNumber(), m_memcard_header_data);
}

CEXIChannel::~CEXIChannel()
{
  RemoveDevices();
}

u32 CEXIChannel::ReadParameterRegister()
{
  // The "device present" bit reflects whatever sits on chip select 0. Channel 2 has no
  // external connector to report.
  const bool present = HasExternalInterruptLine() && GetDevice(1)->IsPresent();
  if (present)
    m_param |= PARAM_EXT_PRESENT;
  else
    m_param &= ~PARAM_EXT_PRESENT;
  return m_param;
}

void CEXIChannel::WriteParameterRegister(u32 value)
{
  // Masks and the clock are plain fields; writing 1 to an interrupt bit acknowledges it.
  u32 kept_fields = PARAM_EXI_MASK | PARAM_TC_MASK | PARAM_CLOCK;
  u32 acknowledged = value & (PARAM_EXI_IRQ | PARAM_TC_IRQ);
  if (HasExternalInterruptLine())
  {
    kept_fields |= PARAM_EXT_MASK;
    acknowledged |= value & PARAM_EXT_IRQ;
  }
  if (m_channel_id == 0)
    kept_fields |= PARAM_ROM_DISABLE;

  m_param = (m_param & ~kept_fields) | (value & kept_fields);
  m_param &= ~acknowledged;

  // A device only hears about the change when exactly one chip select line toggled.
  const u8 old_cs = ChipSelectOf(m_param);
  const u8 new_cs = ChipSelectOf(value);
  IEXIDevice* toggled = GetDevice(static_cast<u8>(old_cs ^ new_cs));
  m_param = (m_param & ~PARAM_CS) | (u32{new_cs} << PARAM_CS_SHIFT);
  if (toggled)
    toggled->SetCS(new_cs);

  m_system.GetExpansionInterface().UpdateInterrupts();
}

void CEXIChannel::RunTransfer(IEXIDevice& device)
{
  const Direction direction = ControlDirection(m_control);

  if ((m_control & CTRL_USE_DMA) == 0)
  {
    const u32 length = ControlImmediateLength(m_control);
    if (direction == Direction::Read)
      m_immediate = device.ImmRead(length);
    else if (direction == Direction::Write)
      device.ImmWrite(m_immediate, length);
    else if (direction == Direction::ReadWrite)
      device.ImmReadWrite(m_immediate, length);
    else
      DEBUG_ASSERT_MSG(EXPANSIONINTERFACE, 0, "EXI Imm: Unknown transfer type {}",
                       static_cast<u32>(direction));
    return;
  }

  if (direction == Direction::Read)
    device.DMARead(m_dma_address, m_dma_length);
  else if (direction == Direction::Write)
    device.DMAWrite(m_dma_address, m_dma_length);
  else
    DEBUG_ASSERT_MSG(EXPANSIONINTERFACE, 0, "EXI DMA: Unknown transfer type {}",
                     static_cast<u32>(direction));
}

void CEXIChannel::WriteControlRegister(u32 value)
{
  m_control = value;
  if ((m_control & CTRL_START) == 0)
    return;

  // With nothing selected the start bit simply stays set.
  IEXIDevice* device = GetDevice(ChipSelectOf(m_param));
  if (device == nullptr)
    return;

  RunTransfer(*device);
  m_control &= ~CTRL_START;

  // Devices with their own timing raise transfer-complete themselves.
  if (!device->UseDelayedTransferCompletion())
    SendTransferComplete();
}

void CEXIChannel::RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  // base is only 4-byte aligned here (see ExpansionInterfaceManager::RegisterMMIO).
  mmio->Register(
      base + REG_PARAMETER,
      MMIO::ComplexRead<u32>([this](Core::System&, u32) { return ReadParameterRegister(); }),
      MMIO::ComplexWrite<u32>(
          [this](Core::System&, u32, u32 value) { WriteParameterRegister(value); }));

  mmio->Register(base + REG_DMA_ADDRESS, MMIO::DirectRead<u32>(&m_dma_address),
                 MMIO::DirectWrite<u32>(&m_dma_address));
  mmio->Register(base + REG_DMA_LENGTH, MMIO::DirectRead<u32>(&m_dma_length),
                 MMIO::DirectWrite<u32>(&m_dma_length));
  mmio->Register(base + REG_CONTROL, MMIO::DirectRead<u32>(&m_control),
                 MMIO::ComplexWrite<u32>(
                     [this](Core::System&, u32, u32 value) { WriteControlRegister(value); }));
  mmio->Register(base + REG_IMMEDIATE, MMIO::DirectRead<u32>(&m_immediate),
                 MMIO::DirectWrite<u32>(&m_immediate));
}

void CEXIChannel::SendTransferComplete()
{
  m_param |= PARAM_TC_IRQ;
  m_system.GetExpansionInterface().UpdateInterrupts();
}

void CEXIChannel::RemoveDevices()
{
  for (std::unique_ptr<IEXIDevice>& slot : m_devices)
    slot.reset();
}

void CEXIChannel::AddDevice(const EXIDeviceType device_type, const int device_num)
{
  AddDevice(EXIDevice_Create(m_system, device_type, ChannelNumber(), m_memcard_header_data),
            device_num);
}

void CEXIChannel::AddDevice(std::unique_ptr<IEXIDevice> device, const int device_num,
                            bool notify_presence_changed)
{
  DEBUG_ASSERT(device_num < DEVICES_PER_CHANNEL);

  INFO_LOG_FMT(EXPANSIONINTERFACE, "EXI channel {} device {}: now type {}, software told: {}",
               m_channel_id, device_num, static_cast<int>(device->m_device_type),
               notify_presence_changed);

  m_devices[static_cast<std::size_t>(device_num)] = std::move(device);

  // Software learns about insertions and removals through the external-insertion interrupt and
  // then reads the "device present" bit. Channel 2 has no such line.
  if (notify_presence_changed && HasExternalInterruptLine())
  {
    m_param |= PARAM_EXT_IRQ;
    m_system.GetExpansionInterface().UpdateInterrupts();
  }
}

bool CEXIChannel::IsCausingInterrupt()
{
  // The memory card slot's interrupt is always seen; otherwise the selected device's.
  if (HasExternalInterruptLine() && GetDevice(1)->IsInterruptSet())
  {
    m_param |= PARAM_EXI_IRQ;
  }
  else
  {
    IEXIDevice* selected = GetDevice(ChipSelectOf(m_param));
    if (selected && selected->IsInterruptSet())
      m_param |= PARAM_EXI_IRQ;
  }

  return AllSet(m_param, PARAM_EXI_IRQ | PARAM_EXI_MASK) ||
         AllSet(m_param, PARAM_TC_IRQ | PARAM_TC_MASK) ||
         AllSet(m_param, PARAM_EXT_IRQ | PARAM_EXT_MASK);
}

IEXIDevice* CEXIChannel::GetDevice(const u8 chip_select)
{
  if (chip_select == 1)
    return m_devices[0].get();
  if (chip_select == 2)
    return m_devices[1].get();
  if (chip_select == 4)
    return m_devices[2].get();
  return nullptr;
}

void CEXIChannel::DoState(PointerWrap& p)
{
  p.Do(m_param);
  p.Do(m_dma_address);
  p.Do(m_dma_length);
  p.Do(m_control);
  p.Do(m_immediate);

  const Memcard::HeaderData header_before = m_memcard_header_data;
  p.Do(m_memcard_header_data);

  for (int index = 0; index < DEVICES_PER_CHANNEL; ++index)
  {
    std::unique_ptr<IEXIDevice>& current = m_devices[static_cast<std::size_t>(index)];
    EXIDeviceType saved_type = current->m_device_type;
    p.Do(saved_type);

    if (saved_type == current->m_device_type)
    {
      current->DoState(p);
    }
    else
    {
      // The savestate has a different device here: build one of that type, load it, and put
      // it in without telling software (the state already reflects it).
      std::unique_ptr<IEXIDevice> loaded =
          EXIDevice_Create(m_system, saved_type, ChannelNumber(), m_memcard_header_data);
      loaded->DoState(p);
      AddDevice(std::move(loaded), index, false);
    }

    // A GCI folder card from the savestate with other format parameters than the card in use
    // would look like a foreign card to the game. Pull it out now and plug in a fresh folder
    // card (built from the loaded parameters) after the usual swap delay, which also keeps any
    // in-flight writes away from the folder. Movies must stay deterministic, so they skip this.
    const bool folder_card_changed = saved_type == EXIDeviceType::MemoryCardFolder &&
                                     header_before != m_memcard_header_data;
    if (folder_card_changed && !m_system.GetMovie().IsMovieActive())
    {
      AddDevice(EXIDeviceType::None, index);
      m_system.GetExpansionInterface().ChangeDevice(static_cast<u8>(m_channel_id),
                                                    static_cast<u8>(index),
                                                    EXIDeviceType::MemoryCardFolder,
                                                    CoreTiming::FromThread::CPU);
    }
  }
}

void CEXIChannel::SetEXIINT(bool exiint)
{
  if (exiint)
    m_param |= PARAM_EXI_IRQ;
  else
    m_param &= ~PARAM_EXI_IRQ;
}
}  // namespace ExpansionInterface
