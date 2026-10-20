// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/HW/Wiimote.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Bluetooth/BTBase.h"
#include "Core/IOS/USB/Bluetooth/WiimoteDevice.h"
#include "Core/IOS/USB/Bluetooth/hci.h"
#include "Core/IOS/USB/USBV0.h"

class PointerWrap;

namespace IOS::HLE
{
// An HCI event waiting for the software to hand over an event buffer. Saved raw in savestates,
// so the layout (1032 bytes) must not change.
struct SQueuedEvent
{
  u8 buffer[1024] = {};
  u32 size = 0;
  u16 connection_handle = 0;
  // Spelled out so that savestates do not pick up stale bytes.
  u16 padding = 0;

  SQueuedEvent(u32 size_, u16 handle);
  SQueuedEvent() = default;
};
static_assert(sizeof(SQueuedEvent) == 1032);

// /dev/usb/oh1/57e/305 when the Bluetooth module is emulated: an HLE Bluetooth controller that
// talks HCI over USB (control messages for commands, an interrupt endpoint for events, bulk
// endpoints for ACL data) and has up to four Wii Remotes and a balance board attached.
class BluetoothEmuDevice final : public BluetoothBaseDevice
{
public:
  BluetoothEmuDevice(EmulationKernel& ios, const std::string& device_name);
  ~BluetoothEmuDevice() override;

  std::optional<IPCReply> Close(u32 fd) override;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;

  void Update() override;

  // ACL data from a remote to the software.
  void SendACLPacket(const bdaddr_t& source, const u8* data, u32 size);

  // A remote asks for a connection. Returns false when page scanning is off, as the controller
  // then cannot hear it.
  bool RemoteConnect(WiimoteDevice&);
  bool RemoteDisconnect(const bdaddr_t& address);

  WiimoteDevice* AccessWiimoteByIndex(std::size_t index);

  void DoState(PointerWrap& p) override;

private:
  // ACL data that arrived while the software had no ACL buffer to give. Saved raw in
  // savestates: keep the layout.
  struct PendingACL
  {
    u8 data[ACL_PKT_SIZE];
    u16 size;
    u16 conn_handle;
  };
  static_assert(sizeof(PendingACL) == 344);

  std::array<std::unique_ptr<WiimoteDevice>, MAX_BBMOTES> m_wiimotes;

  bdaddr_t m_controller_bd{{0x11, 0x02, 0x19, 0x79, 0x00, 0xff}};

  // Write Scan Enable bits. Remotes may only connect while page scanning is on.
  u8 m_scan_enable = 0;

  std::unique_ptr<USB::V0IntrMessage> m_hci_endpoint;
  std::unique_ptr<USB::V0BulkMessage> m_acl_endpoint;
  std::deque<SQueuedEvent> m_event_queue;
  std::deque<PendingACL> m_pending_acl;

  // ACL packets each remote received since the last Number Of Completed Packets event.
  u32 m_packet_count[MAX_BBMOTES] = {};
  u64 m_last_ticks = 0;

  // The handle of a remote's connection is fixed: 0x100 + its slot.
  static u16 GetConnectionHandle(const bdaddr_t&);
  static u32 GetWiimoteNumberFromConnectionHandle(u16 connection_handle);

  WiimoteDevice* AccessWiimote(const bdaddr_t& address);
  WiimoteDevice* AccessWiimote(u16 connection_handle);

  // ACL data.
  void HandleOutgoingACL(const USB::V0BulkMessage& message);
  void WriteACL(const USB::V0BulkMessage& endpoint, u16 connection_handle, const u8* data,
                u16 size);
  void QueueACL(const u8* data, u16 size, u16 connection_handle);

  // Events.
  void DeliverEvent(const SQueuedEvent& event);
  void AddEventToQueue(const SQueuedEvent& event);
  void SendCommandStatus(u16 opcode);
  void SendCommandComplete(u16 opcode, std::span<const u8> return_params);
  void SendInquiryResult();
  void SendInquiryComplete(u8 num_responses);
  void SendRemoteName(const bdaddr_t& bd);
  void SendConnectionRequest(const WiimoteDevice& wiimote);
  void SendConnectionComplete(const bdaddr_t& bd, u8 status);
  void SendClockOffset(u16 connection_handle);
  void SendPacketTypeChanged(u16 connection_handle, u16 packet_type);
  void SendRemoteVersion(u16 connection_handle);
  void SendRemoteFeatures(u16 connection_handle);
  void SendRoleChange(const bdaddr_t& bd, bool master);
  void SendCompletedPackets();
  void SendAuthenticationComplete(u16 connection_handle);
  void SendModeChange(u16 connection_handle, u8 mode, u16 interval);
  bool SendDisconnectionComplete(u16 connection_handle, u8 reason);
  void SendReturnLinkKeys(u8 count);

  // HCI commands.
  void ExecuteHCICommandMessage(const USB::V0CtrlMessage& ctrl_message);
  void UpdateInputs(u64 now);
};
}  // namespace IOS::HLE
