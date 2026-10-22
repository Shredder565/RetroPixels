// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>

#include "Common/CommonTypes.h"

#include "Core/HW/GCMemcard/GCMemcard.h"

class PointerWrap;

namespace MMIO
{
class Mapping;
}

namespace ExpansionInterface
{
class IEXIDevice;
enum class EXIDeviceType : int;

// One EXI channel: its five registers (parameter/status, DMA address, DMA length, control,
// immediate data) and the devices on its three chip selects.
class CEXIChannel
{
public:
  explicit CEXIChannel(Core::System& system, u32 channel_id,
                       const Memcard::HeaderData& memcard_header_data);
  ~CEXIChannel();

  // Returns the device for a one-hot chip select value (1, 2 or 4), or nullptr.
  IEXIDevice* GetDevice(u8 chip_select);

  void RegisterMMIO(MMIO::Mapping* mmio, u32 base);

  void SendTransferComplete();

  void AddDevice(EXIDeviceType device_type, int device_num);
  void AddDevice(std::unique_ptr<IEXIDevice> device, int device_num,
                 bool notify_presence_changed = true);

  void RemoveDevices();

  bool IsCausingInterrupt();
  void DoState(PointerWrap& p);

  // Only used to route serial port 1's interrupt line to channel 2.
  void SetEXIINT(bool exiint);

private:
  static constexpr int DEVICES_PER_CHANNEL = 3;

  u32 ReadParameterRegister();
  void WriteParameterRegister(u32 value);
  void WriteControlRegister(u32 value);
  void RunTransfer(IEXIDevice& device);

  bool HasExternalInterruptLine() const { return m_channel_id != 2; }
  int ChannelNumber() const { return static_cast<int>(m_channel_id); }

  Core::System& m_system;

  // Saved in this order, as raw words. The parameter register keeps the hardware bit layout:
  // 0 EXI irq mask, 1 EXI irq, 2 transfer-complete mask, 3 transfer complete, 4-6 clock,
  // 7-9 chip select, 10 external-insertion mask, 11 external insertion, 12 device present,
  // 13 ROM descrambler disable. The control register: 0 start, 1 DMA, 2-3 direction, 4-5 length.
  u32 m_param = 0;
  u32 m_dma_address = 0;
  u32 m_dma_length = 0;
  u32 m_control = 0;
  u32 m_immediate = 0;

  u32 m_channel_id;

  // The card formatting parameters for memory cards created on this channel. A GCI folder card
  // needs them again whenever it is re-created (device swap, savestate load).
  Memcard::HeaderData m_memcard_header_data;

  std::array<std::unique_ptr<IEXIDevice>, DEVICES_PER_CHANNEL> m_devices;
};
}  // namespace ExpansionInterface
