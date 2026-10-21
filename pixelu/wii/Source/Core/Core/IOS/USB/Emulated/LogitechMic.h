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
// Settings the game controls through USB Audio Class requests.
class LogitechMicState final : public MicrophoneState
{
public:
  // Read by the capture callback on another thread.
  std::atomic<bool> mute = false;
  std::atomic<u8> volume = 0;
  std::atomic<u32> sample_rate = DEFAULT_SAMPLING_RATE;

  static constexpr u32 DEFAULT_SAMPLING_RATE = 48000;

  bool IsSampleOn() const override;
  bool IsMuted() const override;
  u32 GetDefaultSamplingRate() const override;
};

// The Logitech USB microphone (046d:0a03) used by Rock Band and other games: a standard USB
// Audio Class 1.0 device with one mono 16-bit input. Up to four can be emulated (index 0-3).
class LogitechMic final : public Device
{
public:
  explicit LogitechMic(u8 index);

  DeviceDescriptor GetDeviceDescriptor() const override;
  std::vector<ConfigDescriptor> GetConfigurations() const override;
  std::vector<InterfaceDescriptor> GetInterfaces(u8 config) const override;
  std::vector<EndpointDescriptor> GetEndpoints(u8 config, u8 iface, u8 alt) const override;
  bool Attach() override;
  bool AttachAndChangeInterface(u8 iface) override;
  int CancelTransfer(u8 endpoint) override;
  int ChangeInterface(u8 iface) override;
  int GetNumberOfAltSettings(u8 iface) override;
  int SetAltSetting(u8 alt_setting) override;
  int SubmitTransfer(std::unique_ptr<CtrlMessage> cmd) override;
  int SubmitTransfer(std::unique_ptr<BulkMessage> cmd) override;
  int SubmitTransfer(std::unique_ptr<IntrMessage> cmd) override;
  int SubmitTransfer(std::unique_ptr<IsoMessage> cmd) override;

private:
  // Audio Class requests. Each returns the number of bytes answered, 0 for an accepted
  // request without data, or IPC_STALL.
  int FeatureUnitGet(const CtrlMessage& cmd) const;
  int FeatureUnitSet(const CtrlMessage& cmd);
  int EndpointRequest(const CtrlMessage& cmd);

  LogitechMicState m_sampler{};
  std::unique_ptr<Microphone> m_microphone;
  u8 m_index = 0;
  u8 m_active_interface = 0;
  bool m_device_attached = false;
};
}  // namespace IOS::HLE::USB
