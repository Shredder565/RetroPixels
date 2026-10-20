// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <SFML/Network.hpp>
#ifdef HAVE_CPPIPC
#include <libipc/ipc.h>
#endif

#include "Common/Flag.h"
#include "Common/Network.h"
#include "Core/HW/EXI/BBA/BuiltIn.h"
#include "Core/HW/EXI/BBA/TAPServerConnection.h"
#include "Core/HW/EXI/EXI_Device.h"

class PointerWrap;

namespace CoreTiming
{
enum class FromThread;
}

namespace ExpansionInterface
{
// Largest frame the adapter accepts from a backend.
constexpr u32 BBA_RECV_SIZE = 0x800;

enum class BBADeviceType
{
  TAP,
  XLINK,
  TAPSERVER,
  BuiltIn,
  IPC,
};

// The Broadband Adapter (DOL-015): an EXI front end with a few registers of its own, in front of
// a Macronix MX98728EC network controller whose 4 KiB of registers and receive pages are reached
// through EXI commands. Frames go out through the controller's direct transmit FIFO and come in
// through its ring of 256-byte receive pages. The host side of the network is one of several
// backends.
class CEXIETHERNET : public IEXIDevice
{
public:
  CEXIETHERNET(Core::System& system, BBADeviceType type);
  ~CEXIETHERNET() override;
  void SetCS(int cs) override;
  bool IsPresent() const override;
  bool IsInterruptSet() override;
  void ImmWrite(u32 data, u32 size) override;
  u32 ImmRead(u32 size) override;
  void DMAWrite(u32 addr, u32 size) override;
  void DMARead(u32 addr, u32 size) override;
  void DoState(PointerWrap& p) override;

private:
  // What a backend implements. Backends call back into the adapter with ReceiveFrame and
  // SendComplete.
  class NetworkInterface
  {
  public:
    virtual ~NetworkInterface() = default;

    virtual bool Activate() { return false; }
    virtual void Deactivate() {}
    virtual bool IsActivated() { return false; }
    virtual bool SendFrame(const u8*, u32) { return false; }
    virtual bool RecvInit() { return false; }
    virtual void RecvStart() {}
    virtual void RecvStop() {}

  protected:
    explicit NetworkInterface(CEXIETHERNET* eth_ref) : m_eth_ref{eth_ref} {}
    CEXIETHERNET* m_eth_ref = nullptr;
  };

  // A TAP-Windows virtual adapter (OpenVPN's driver).
  class TAPNetworkInterface : public NetworkInterface
  {
  public:
    explicit TAPNetworkInterface(CEXIETHERNET* eth_ref) : NetworkInterface(eth_ref) {}

    bool Activate() override;
    void Deactivate() override;
    bool IsActivated() override;
    bool SendFrame(const u8* frame, u32 size) override;
    bool RecvInit() override;
    void RecvStart() override;
    void RecvStop() override;

  private:
#ifdef _WIN32
    void ReadLoop();

    HANDLE m_adapter = INVALID_HANDLE_VALUE;
    OVERLAPPED m_read_io = {};
    OVERLAPPED m_write_io = {};
    std::vector<u8> m_write_buffer;
    bool m_write_in_flight = false;
    std::array<u8, BBA_RECV_SIZE> m_read_buffer{};
    std::thread m_read_thread;
    Common::Flag m_receiving;
    Common::Flag m_stop_reading;
#endif
  };

  // A tapserver connection (TCP, or a UNIX socket elsewhere).
  class TAPServerNetworkInterface : public NetworkInterface
  {
  public:
    TAPServerNetworkInterface(CEXIETHERNET* eth_ref, const std::string& destination);

    bool Activate() override;
    void Deactivate() override;
    bool IsActivated() override;
    bool SendFrame(const u8* frame, u32 size) override;
    bool RecvInit() override;
    void RecvStart() override;
    void RecvStop() override;

  private:
    void OnFrame(std::string&& frame);

    TAPServerConnection m_connection;
  };

  // The XLink Kai client's emulator UDP protocol.
  class XLinkNetworkInterface : public NetworkInterface
  {
  public:
    XLinkNetworkInterface(CEXIETHERNET* eth_ref, std::string dest_ip, int dest_port,
                          std::string identifier, bool chat_osd_enabled)
        : NetworkInterface(eth_ref), m_dest_ip(std::move(dest_ip)), m_dest_port(dest_port),
          m_client_identifier(std::move(identifier)), m_chat_osd_enabled(chat_osd_enabled)
    {
    }

    bool Activate() override;
    void Deactivate() override;
    bool IsActivated() override;
    bool SendFrame(const u8* frame, u32 size) override;
    bool RecvInit() override;
    void RecvStart() override;
    void RecvStop() override;

  private:
    // Large enough for the jumbo frames XLink Kai may relay.
    static constexpr std::size_t DATAGRAM_SIZE = 9004;

    void ReadLoop();
    bool SendDatagram(const void* data, std::size_t size);
    void SendCommand(std::string_view command);
    void HandleControlMessage(std::size_t size);

    std::string m_dest_ip;
    int m_dest_port;
    std::string m_client_identifier;
    bool m_chat_osd_enabled;
    bool m_link_up = false;
    bool m_told_user_not_connected = false;
    sf::UdpSocket m_socket;
    sf::IpAddress m_peer = sf::IpAddress::Any;
    std::array<char, DATAGRAM_SIZE> m_in{};
    std::array<char, DATAGRAM_SIZE> m_out{};
    std::thread m_read_thread;
    Common::Flag m_receiving;
    Common::Flag m_stop_reading;
  };

  // A small built-in network stack (ARP, DHCP, UDP and TCP relayed through host sockets).
  class BuiltInBBAInterface : public NetworkInterface
  {
  public:
    BuiltInBBAInterface(CEXIETHERNET* eth_ref, std::string dns_ip, std::string local_ip)
        : NetworkInterface(eth_ref), m_dns_ip(std::move(dns_ip)), m_local_ip(std::move(local_ip))
    {
    }

    bool Activate() override;
    void Deactivate() override;
    bool IsActivated() override;
    bool SendFrame(const u8* frame, u32 size) override;
    bool RecvInit() override;
    void RecvStart() override;
    void RecvStop() override;

  private:
    static constexpr u8 QUEUE_LENGTH = 16;

    void ReadLoop();
    // One pass of the reader: retransmits, polls the host sockets and hands at most one frame
    // to the adapter. Returns whether a frame was delivered.
    bool PumpOnce();

    // Frames waiting for the console, in a ring of QUEUE_LENGTH slots.
    void Enqueue(std::vector<u8> frame);
    bool QueueFull() const;
    bool Dequeue(std::vector<u8>* frame);

    void CollectSocketData(std::vector<u8>* frame);
    std::optional<std::vector<u8>> ReceiveFromSocket(BbaConnection& connection);
    std::optional<std::vector<u8>> ReceiveUdp(BbaConnection& connection);
    std::optional<std::vector<u8>> ReceiveTcp(BbaConnection& connection);

    void OnARP(const Common::ARPPacket& packet);
    void OnDHCP(const Common::UDPPacket& packet);
    void OnTCP(const Common::TCPPacket& packet);
    void OnTCPData(BbaConnection& connection, const Common::TCPPacket& packet);
    void OpenTCP(const Common::TCPPacket& packet);
    void OnUDP(const Common::UDPPacket& packet);
    void OpenListeningUdpPort(u16 port);
    void AcceptUPnPClient();
    const Common::MACAddress& MacFor(u32 inet_ip);

    std::string m_dns_ip;
    std::string m_local_ip;
    bool m_active = false;
    u16 m_ip_id = 0;

    std::mutex m_lock;
    std::array<std::vector<u8>, QUEUE_LENGTH> m_queue;
    u8 m_queue_head = 0;
    u8 m_queue_tail = 0;

    // Addresses in network byte order.
    u32 m_console_ip = 0;
    Common::MACAddress m_console_mac{};
    u32 m_gateway_ip = 0;
    Common::MACAddress m_gateway_mac{};
    std::map<u32, Common::MACAddress> m_arp;

    BbaConnections m_connections;
    sf::TcpListener m_upnp_listener;

    std::thread m_read_thread;
    Common::Flag m_receiving;
    Common::Flag m_stop_reading;
  };

  // Frames exchanged with another process through a cpp-ipc channel.
  class IPCBBAInterface : public NetworkInterface
  {
  public:
    explicit IPCBBAInterface(CEXIETHERNET* const eth_ref) : NetworkInterface(eth_ref) {}

#ifdef HAVE_CPPIPC
    bool Activate() override;
    void Deactivate() override;
    bool IsActivated() override;
    bool SendFrame(const u8* frame, u32 size) override;
    bool RecvInit() override;
    void RecvStart() override;
    void RecvStop() override;

  private:
    void ReadLoop();

    bool m_active = false;
    ipc::channel m_channel;
    std::thread m_read_thread;
    Common::Flag m_receiving;
    Common::Flag m_stop_reading;
#endif
  };

  // What the current command selected.
  enum class Region : u8
  {
    Adapter,
    Controller,
  };
  struct Transfer
  {
    bool valid = false;
    bool write = false;
    Region region = Region::Adapter;
    u16 address = 0;
  };

  // Controller memory helpers (little-endian 16-bit registers).
  u8& Reg(u32 address) { return m_mem[address & (MEMORY_SIZE - 1)]; }
  u16 Reg16(u32 address) const;
  void SetReg16(u32 address, u16 value);

  void StartTransfer(u32 command);
  void WriteAdapterRegister(u32 value);
  u32 ReadAdapterRegister(u32 size);
  void WriteController(u32 data, u32 size);
  u32 WriteControlA(u32 value);
  void PushTransmitFifo(const u8* data, u32 size);
  void TransmitFifo();
  void RaiseInterrupt(u8 cause, bool schedule, CoreTiming::FromThread from);

  // Backend callbacks.
  void SendComplete();
  void ReceiveFrame(const u8* frame, u32 size);
  bool AcceptsDestination() const;
  void StoreReceivedFrame();
  Common::MACAddress MacAddress() const;
  u32 ReceivePagesInUse() const;

  static constexpr u32 MEMORY_SIZE = 0x1000;
  static constexpr u32 TX_FIFO_SIZE = 1518;

  Transfer m_transfer;

  // Adapter registers.
  u8 m_irq_mask = 0;
  std::atomic<u8> m_irq_cause = 0;

  // Controller state, saved in this order.
  std::unique_ptr<u8[]> m_tx_fifo;
  std::unique_ptr<u8[]> m_mem;

  std::unique_ptr<u8[]> m_rx_frame;
  u32 m_rx_length = 0;

  std::unique_ptr<NetworkInterface> m_network_interface;
};
}  // namespace ExpansionInterface
