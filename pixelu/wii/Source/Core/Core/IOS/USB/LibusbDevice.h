// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#if defined(__LIBUSB__)
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/IOS/USB/Common.h"
#include "Core/LibusbUtils.h"

struct libusb_device;
struct libusb_device_descriptor;
struct libusb_device_handle;
struct libusb_transfer;

namespace IOS::HLE::USB
{
// A real USB device of the host, passed through to the guest with libusb.
class LibusbDevice final : public Device
{
public:
  LibusbDevice(libusb_device* device, const libusb_device_descriptor& device_descriptor);
  ~LibusbDevice() override;
  DeviceDescriptor GetDeviceDescriptor() const override;
  std::vector<ConfigDescriptor> GetConfigurations() const override;
  std::vector<InterfaceDescriptor> GetInterfaces(u8 config) const override;
  std::vector<EndpointDescriptor> GetEndpoints(u8 config, u8 iface, u8 alt) const override;
  std::string GetErrorName(int error_code) const override;
  bool Attach() override;
  bool AttachAndChangeInterface(u8 iface) override;
  int CancelTransfer(u8 endpoint) override;
  int ChangeInterface(u8 iface) override;
  int GetNumberOfAltSettings(u8 iface) override;
  int SetAltSetting(u8 alt_setting) override;
  int SubmitTransfer(std::unique_ptr<CtrlMessage> message) override;
  int SubmitTransfer(std::unique_ptr<BulkMessage> message) override;
  int SubmitTransfer(std::unique_ptr<IntrMessage> message) override;
  int SubmitTransfer(std::unique_ptr<IsoMessage> message) override;

private:
  // Transfers in flight on one endpoint, keyed by their libusb transfer.
  class PendingTransfers final
  {
  public:
    void Add(libusb_transfer* transfer, std::unique_ptr<TransferCommand> command);
    // Answers and forgets the command of a finished transfer. `on_success` copies the data
    // back and gives the reply value when the transfer completed normally.
    void Finish(libusb_transfer* transfer,
                const std::function<s32(const TransferCommand&)>& on_success);
    void CancelAll();

  private:
    std::mutex m_lock;
    std::map<libusb_transfer*, std::unique_ptr<TransferCommand>> m_commands;
  };

  static void OnControlTransferDone(libusb_transfer* transfer);
  static void OnTransferDone(libusb_transfer* transfer);

  // Fills in the transfer fields shared by bulk, interrupt and isochronous transfers, queues it
  // and submits it.
  int Queue(libusb_transfer* transfer, std::unique_ptr<TransferCommand> command);

  void ApplyPlayStationDisguise();
  int SendRockBand3InstrumentEnable();

  int ForEachInterface(u8 config_num, const std::function<int(u8)>& action) const;
  int ClaimAllInterfaces(u8 config_num) const;
  int ReleaseAllInterfaces(u8 config_num) const;
  int ReleaseInterfacesOfActiveConfig() const;

  std::vector<LibusbUtils::ConfigDescriptor> m_config_descriptors;
  u16 m_vid = 0;
  u16 m_pid = 0;
  // What the guest is told; differs from the real IDs for disguised PlayStation instruments.
  u16 m_reported_vid = 0;
  u16 m_reported_pid = 0;
  u8 m_active_interface = 0;
  bool m_device_attached = false;
  bool m_needs_rb3_instrument_enable = false;

  libusb_device* m_device = nullptr;
  libusb_device_handle* m_handle = nullptr;

  std::map<u8, PendingTransfers> m_pending;
};
}  // namespace IOS::HLE::USB
#endif
