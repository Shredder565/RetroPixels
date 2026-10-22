// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#if defined(__LIBUSB__)
#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/Timer.h"

#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Bluetooth/BTBase.h"
#include "Core/IOS/USB/Bluetooth/LibUSBBluetoothAdapter.h"
#include "Core/IOS/USB/Bluetooth/hci.h"
#include "Core/IOS/USB/USBV0.h"

class PointerWrap;

namespace IOS::HLE
{
// The red sync button on the console.
enum class SyncButtonState
{
  Unpressed,
  Held,
  Pressed,
  LongPressed,
  // A real Wii ignores the release that ends a long press: it triggers no sync.
  Ignored,
};

using linkkey_t = std::array<u8, 16>;

// /dev/usb/oh1/57e/305 with Bluetooth passthrough: the emulated software's HCI traffic goes to a
// real USB Bluetooth adapter. A few things are patched along the way so that adapters other
// than the Wii's own module behave like it.
class BluetoothRealDevice final : public BluetoothBaseDevice
{
public:
  BluetoothRealDevice(EmulationKernel& ios, const std::string& device_name);
  ~BluetoothRealDevice() override;

  std::optional<IPCReply> Open(const OpenRequest& request) override;
  std::optional<IPCReply> Close(u32 fd) override;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;
  void Update() override;

  void DoState(PointerWrap& p) override;
  void UpdateSyncButtonState(bool is_held) override;
  void TriggerSyncButtonPressedEvent() override;
  void TriggerSyncButtonHeldEvent() override;

private:
  using BufferType = LibUSBBluetoothAdapter::BufferType;
  // Writes a made-up event into the software's event buffer and answers it.
  using FakeEvent = std::function<void(USB::V0IntrMessage&)>;

  // Holding the button this long resets the console's pairings.
  static constexpr u32 SYNC_BUTTON_HOLD_MS_TO_RESET = 10000;

  std::unique_ptr<LibUSBBluetoothAdapter> m_adapter;

  std::atomic<SyncButtonState> m_sync_button_state{SyncButtonState::Unpressed};
  Common::Timer m_sync_button_timer;

  // Replies to commands that are not sent to the adapter, in order.
  std::queue<FakeEvent> m_fake_events;

  // The link keys of paired devices. The Wii expects its Bluetooth module to remember them, but
  // some adapters forget them on reset, so they are kept here (and in the settings) as well.
  std::map<bdaddr_t, linkkey_t> m_link_keys;

  // Buffers given by the emulated software.
  std::unique_ptr<USB::V0IntrMessage> m_hci_endpoint;
  std::unique_ptr<USB::V0BulkMessage> m_acl_endpoint;

  // When data sent now should reach the adapter, which keeps e.g. speaker data evenly timed.
  TimePoint GetTargetTime() const;

  void HandleCommand(const USB::V0CtrlMessage& command);
  void TryToFillHCIEndpoint();
  void TryToFillACLEndpoint();
  [[nodiscard]] BufferType ProcessHCIEvent(BufferType event);

  void SendCommand(std::span<const u8> command);
  void SendReset();
  void SendDeleteAllLinkKeys();
  void SendStoredLinkKeys();

  void WriteEvent(USB::V0IntrMessage& endpoint, u8 code, std::span<const u8> params);
  void FakeVendorCommandReply(u16 opcode, USB::V0IntrMessage& endpoint);
  void FakeSyncButtonEvent(USB::V0IntrMessage& endpoint, u8 kind);

  void LoadLinkKeys();
  void SaveLinkKeys();
};
}  // namespace IOS::HLE

#else
#include "Core/IOS/USB/Bluetooth/BTStub.h"

namespace IOS::HLE
{
using BluetoothRealDevice = BluetoothStubDevice;
}  // namespace IOS::HLE
#endif
