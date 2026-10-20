// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include <SFML/Network.hpp>

#include "Common/CommonTypes.h"
#include "Core/HW/SI/SI_Device.h"

// Game Boy Advance on the link cable, provided by an external emulator that
// connects over TCP: port 0xd6ba carries the joybus bytes and port 0xc10c a
// clock stream telling the GBA side how much time has passed.

namespace SerialInterface
{
// Stops the background thread that accepts emulator connections.
void GBAConnectionWaiter_Shutdown();

class GBASockServer
{
public:
  GBASockServer();
  ~GBASockServer();

  bool Connect();
  bool IsConnected();
  void ClockSync(Core::System& system);
  void Send(const u8* si_buffer);
  int Receive(u8* si_buffer, u8 bytes);
  void Flush();

private:
  void Disconnect();

  std::unique_ptr<sf::TcpSocket> m_data_socket;
  std::unique_ptr<sf::TcpSocket> m_clock_socket;

  // Tick count of the last clock message (0 = none sent yet).
  u64 m_last_clock_ticks = 0;
  // Set once the other side has answered; afterwards replies are waited for.
  bool m_peer_answered = false;
};

class CSIDevice_GBA final : public ISIDevice
{
public:
  CSIDevice_GBA(Core::System& system, SIDevices device, int device_number);

  int RunBuffer(u8* buffer, int request_length) override;
  int TransferInterval() override;
  DataResponse GetData(u32& hi, u32& low) override;
  void SendCommand(u32 command, u8 poll) override;

private:
  enum class Phase
  {
    Idle,          // next RunBuffer sends a new command
    InFlight,      // waiting for the bytes to cross the cable
    AwaitReply,    // collecting the reply
  };

  GBASockServer m_link;
  Phase m_phase = Phase::Idle;
  EBufferCommands m_pending_command = EBufferCommands::CMD_STATUS;
  u64 m_sent_at = 0;
};
}  // namespace SerialInterface
