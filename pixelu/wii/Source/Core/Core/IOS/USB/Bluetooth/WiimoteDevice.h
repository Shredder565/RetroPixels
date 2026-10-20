// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <map>
#include <span>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/HW/WiimoteCommon/WiimoteHid.h"
#include "Core/IOS/USB/Bluetooth/hci.h"

class PointerWrap;

namespace WiimoteEmu
{
struct DesiredWiimoteState;
}

namespace IOS::HLE
{
class BluetoothEmuDevice;

// One Wii Remote (or the balance board) as seen from the emulated Bluetooth module: its
// baseband connection, its L2CAP channels (SDP, HID control, HID interrupt) and the SDP server.
// HID traffic is passed to and from the remote's source (an emulated or a real remote).
class WiimoteDevice
{
public:
  using ClassType = std::array<u8, BluetoothHCI::CLASS_SIZE>;
  using FeaturesType = std::array<u8, BluetoothHCI::FEATURES_SIZE>;
  using LinkKeyType = std::array<u8, BluetoothHCI::LINK_KEY_SIZE>;

  WiimoteDevice(BluetoothEmuDevice* host, bdaddr_t bd, unsigned int hid_source_number);
  ~WiimoteDevice();

  WiimoteDevice(const WiimoteDevice&) = delete;
  WiimoteDevice& operator=(const WiimoteDevice&) = delete;
  WiimoteDevice(WiimoteDevice&&) = delete;
  WiimoteDevice& operator=(WiimoteDevice&&) = delete;

  // Drops the connection and every channel.
  void Reset();

  // Called on every update of the Bluetooth module.
  void Update();

  // Called at the remote's report rate (about 200 Hz): PrepareInput gathers the input for this
  // round, and UpdateInput is then called with the result of PrepareInput.
  enum class NextUpdateInputCall
  {
    None,
    Activate,
    Update
  };
  NextUpdateInputCall PrepareInput(WiimoteEmu::DesiredWiimoteState* wiimote_state);
  void UpdateInput(NextUpdateInputCall next_call,
                   const WiimoteEmu::DesiredWiimoteState& wiimote_state);

  void DoState(PointerWrap& p);

  bool IsInquiryScanEnabled() const;
  bool IsPageScanEnabled() const;

  u32 GetNumber() const;

  bool IsSourceValid() const;
  bool IsConnected() const;

  // Connects or disconnects the remote at the user's request (shows an OSD message).
  void Activate(bool ready);

  // An L2CAP packet sent to the remote by the emulated software.
  void ExecuteL2capCmd(u8* ptr, u32 size);
  // An input report produced by the remote's source.
  void InterruptDataInputCallback(u8 hid_type, const u8* data, u32 size);

  // Baseband events from the Bluetooth module. They return false when the remote refuses.
  bool EventConnectionAccept();
  bool EventConnectionRequest();
  void EventDisconnect(u8 reason);

  // nullptr disables the remote.
  void SetSource(WiimoteCommon::HIDWiimote*);

  const bdaddr_t& GetBD() const { return m_bd; }
  const char* GetName() const { return m_name.c_str(); }
  u8 GetLMPVersion() const { return m_lmp_version; }
  u16 GetLMPSubVersion() const { return m_lmp_subversion; }
  // The manufacturer ID the remote reports.
  u16 GetManufactorID() const { return 0x000F; }
  const ClassType& GetClass() const { return m_class; }
  const FeaturesType& GetFeatures() const { return m_features; }
  const LinkKeyType& GetLinkKey() const { return m_link_key; }

private:
  // Saved raw in savestates, so the underlying types and values must not change.
  enum class LinkState : int
  {
    Inactive,
    RequestConnection,
    Complete,
  };

  enum class HIDLinking : int
  {
    Idle,
    // The remote opened the connection, so it also opens its HID channels.
    Opening,
  };

  // One L2CAP channel, keyed by its local channel ID. Saved raw in savestates: keep the layout.
  struct Channel
  {
    enum class Configuration : int
    {
      NotRequested,
      Requested,
      Accepted,
    };

    Configuration configuration = Configuration::NotRequested;
    u16 psm = 0;
    // 0 until the other side has accepted (or opened) the channel.
    u16 remote_cid = 0;
    // 0 until the other side has configured the channel.
    u16 remote_mtu = 0;

    bool IsOpen() const;
  };

  BluetoothEmuDevice* m_host;
  WiimoteCommon::HIDWiimote* m_hid_source = nullptr;

  // Savestate data, in savestate order.
  LinkState m_link_state = LinkState::Inactive;
  HIDLinking m_hid_linking = HIDLinking::Idle;
  bdaddr_t m_bd;
  ClassType m_class{};
  FeaturesType m_features{};
  u8 m_lmp_version = 0;
  u16 m_lmp_subversion = 0;
  LinkKeyType m_link_key{};
  std::string m_name;
  std::map<u16, Channel> m_channels;
  // Counts updates down after a connection change; button presses wake the remote only at 0.
  u8 m_reconnect_delay = 0;

  void SetLinkState(LinkState state);

  Channel* FindChannel(u16 psm);
  const Channel* FindChannel(u16 psm) const;
  u16 FreeChannelID() const;
  bool OpenChannel(u16 psm);

  // L2CAP signaling.
  void HandleSignaling(const u8* data, u32 size);
  void OnConnectionRequest(u8 identifier, const u8* params);
  void OnConnectionResponse(const u8* params);
  void OnConfigureRequest(u8 identifier, const u8* params, u32 length);
  void OnConfigureResponse(const u8* params);
  void OnDisconnectionRequest(u8 identifier, const u8* params);
  void RequestConnection(u16 psm);
  void RequestConfiguration(u16 remote_cid, u16 mtu, u16 flush_timeout);
  void SendSignal(u8 identifier, u8 code, std::span<const u8> params);

  // SDP server.
  void HandleSDP(u16 cid, const u8* data);
  void SendL2CAPPacket(u16 cid, std::span<const u8> payload);
};
}  // namespace IOS::HLE
