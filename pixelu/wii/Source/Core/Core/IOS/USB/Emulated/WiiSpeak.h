// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/IOS/USB/Common.h"
#include "Core/IOS/USB/Emulated/Microphone.h"

namespace IOS::HLE::USB
{
// The sampler settings a game programs into the Wii Speak through its vendor registers.
class WiiSpeakState final : public MicrophoneState
{
public:
  // Read by the capture callback on another thread.
  std::atomic<bool> sample_on = false;
  std::atomic<bool> mute = false;
  int freq = 0;
  int gain = 0;
  bool ec_reset = false;
  bool sp_on = false;

  static constexpr u32 DEFAULT_SAMPLING_RATE = 16000;

  bool IsSampleOn() const override;
  bool IsMuted() const override;
  u32 GetDefaultSamplingRate() const override;
};

// The Wii Speak (057e:0308), a USB microphone for the Wii. Audio goes to the console on an
// isochronous IN endpoint; the sampler is configured with vendor control requests.
class WiiSpeak final : public Device
{
public:
  WiiSpeak();
  ~WiiSpeak() override;

  DeviceDescriptor GetDeviceDescriptor() const override;
  std::vector<ConfigDescriptor> GetConfigurations() const override;
  std::vector<InterfaceDescriptor> GetInterfaces(u8 config) const override;
  std::vector<EndpointDescriptor> GetEndpoints(u8 config, u8 iface, u8 alt) const override;
  bool Attach() override;
  bool AttachAndChangeInterface(u8 f) override;
  int CancelTransfer(u8 endpoint) override;
  int ChangeInterface(u8 iface) override;
  int GetNumberOfAltSettings(u8 iface) override;
  int SetAltSetting(u8 alt_setting) override;
  int SubmitTransfer(std::unique_ptr<CtrlMessage> message) override;
  int SubmitTransfer(std::unique_ptr<BulkMessage> message) override;
  int SubmitTransfer(std::unique_ptr<IntrMessage> message) override;
  int SubmitTransfer(std::unique_ptr<IsoMessage> message) override;

private:
  // Vendor register access: the 6-byte payload is {?, register, value (BE), extra (BE)}.
  void WriteRegister(const CtrlMessage& cmd);
  void ReadRegister(const CtrlMessage& cmd) const;

  WiiSpeakState m_sampler{};
  std::unique_ptr<Microphone> m_microphone;
  u8 m_active_interface = 0;
  bool m_device_attached = false;
  // Flips to true after the first vendor GET_DESCRIPTOR following a vendor reset.
  bool m_ready = false;
};
}  // namespace IOS::HLE::USB
