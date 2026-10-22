// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <string>

#include "Core/HW/EXI/BBA/TAPServerConnection.h"
#include "Core/HW/EXI/EXI_Device.h"

class PointerWrap;

namespace ExpansionInterface
{

static constexpr std::size_t MODEM_RECV_SIZE = 0x800;

enum
{
  EXI_DEVTYPE_MODEM = 0x02020000,
};

enum class ModemDeviceType
{
  TAPSERVER,
};

// The GameCube modem adapter (serial port 1). The game talks to it with a
// transfer descriptor followed by data: either the adapter's registers or its
// AT command and packet buffers. Dialing connects HDLC packet traffic to a
// tapserver.
class CEXIModem : public IEXIDevice
{
public:
  CEXIModem(Core::System& system, ModemDeviceType type);
  ~CEXIModem() override;
  void SetCS(int cs) override;
  bool IsPresent() const override;
  bool IsInterruptSet() override;
  void ImmWrite(u32 data, u32 size) override;
  u32 ImmRead(u32 size) override;
  void DMAWrite(u32 addr, u32 size) override;
  void DMARead(u32 addr, u32 size) override;
  void DoState(PointerWrap& p) override;

private:
  // Called by the network interface.
  void SendComplete();
  void AddToReceiveBuffer(std::string&& data);

  void SetInterrupt(u8 flag, bool on, bool from_cpu);
  void UpdateReceiveState(bool from_cpu);
  void ReadBuffer(u8* destination, u32 size);
  void WriteBuffer(const u8* source, u32 size);
  void AdvanceDescriptor(u32 remaining);
  void RunATCommands();
  void ReplyAT(const std::string& text);

  class NetworkInterface
  {
  protected:
    CEXIModem* m_modem_ref = nullptr;
    explicit NetworkInterface(CEXIModem* modem_ref) : m_modem_ref{modem_ref} {}

  public:
    virtual bool Activate() { return false; }
    virtual void Deactivate() {}
    virtual bool IsActivated() { return false; }
    virtual bool SendAndRemoveAllHDLCFrames(std::string*) { return false; }
    virtual bool RecvInit() { return false; }
    virtual void RecvStart() {}
    virtual void RecvStop() {}

    virtual ~NetworkInterface() = default;
  };

  class TAPServerNetworkInterface : public NetworkInterface
  {
  public:
    TAPServerNetworkInterface(CEXIModem* modem_ref, const std::string& destination);

  public:
    bool Activate() override;
    void Deactivate() override;
    bool IsActivated() override;
    bool SendAndRemoveAllHDLCFrames(std::string* send_buffer) override;
    bool RecvInit() override;
    void RecvStart() override;
    void RecvStop() override;

  private:
    TAPServerConnection m_tapserver_if;

    void HandleReceivedFrame(std::string&& data);
  };

  static constexpr u32 NO_TRANSFER = 0xFFFFFFFF;

  std::unique_ptr<NetworkInterface> m_network_interface;

  u32 m_descriptor = NO_TRANSFER;

  std::string m_at_command;
  std::string m_at_reply;
  std::string m_send_buffer;
  std::mutex m_receive_lock;
  std::string m_receive_buffer;
  std::array<u8, 0x20> m_registers{};
};
}  // namespace ExpansionInterface
