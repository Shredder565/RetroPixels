// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <expected>
#include <span>
#include <vector>

#include "Common/CommonTypes.h"

#include "Core/HW/Triforce/IOPorts.h"
#include "Core/HW/Triforce/SerialDevice.h"

class PointerWrap;

namespace Triforce
{
enum class JVSIOStatusCode : u8;
enum class JVSIOReportCode : u8;
enum class JVSIOCommand : u8;

class JVSIORequestReader;
class JVSIOResponseWriter;

// A JAMMA Video Standard I/O board on the baseboard's JVS bus. The host sends
// addressed, escaped frames; the board answers each unicast frame with one
// response frame holding a report and data for every command in it.
class JVSIOBoard : public SerialDevice
{
public:
  explicit JVSIOBoard(IOPorts* io_ports);

  void Update() override;

  void DoState(PointerWrap& p) override;

protected:
  using HandlerResponse = std::expected<JVSIOReportCode, JVSIOStatusCode>;

  struct FrameContext
  {
    JVSIORequestReader& request;
    JVSIOResponseWriter& response;
  };

  // Handles one command whose code has already been read. Boards override this
  // for their identification and extra commands and pass the rest down.
  virtual HandlerResponse HandleCommand(JVSIOCommand cmd, FrameContext ctx);

private:
  void HandleBroadcast(JVSIORequestReader& request);
  void HandleUnicast(JVSIORequestReader& request);
  void FinishResponse(JVSIOStatusCode status);
  void SendLastResponse();

  // 0 until the host assigns an address.
  u8 m_address = 0;

  // The last response frame without its sync byte, kept for DataRetransmit.
  std::vector<u8> m_last_response;

  std::array<u16, IOPorts::COIN_SLOT_COUNT> m_coins{};
  std::array<bool, IOPorts::COIN_SLOT_COUNT> m_coin_was_down{};

  IOPorts* const m_ports;
};

// Mario Kart Arcade GP and GP2.
class Namco_FCA_JVSIOBoard final : public JVSIOBoard
{
public:
  using JVSIOBoard::JVSIOBoard;

  HandlerResponse HandleCommand(JVSIOCommand cmd, FrameContext ctx) override;
};

// The default board (Virtua Striker and most others).
class Sega_837_13551_JVSIOBoard final : public JVSIOBoard
{
public:
  using JVSIOBoard::JVSIOBoard;

  HandlerResponse HandleCommand(JVSIOCommand cmd, FrameContext ctx) override;
};

// F-Zero AX Deluxe.
class Sega_837_13844_JVSIOBoard final : public JVSIOBoard
{
public:
  using JVSIOBoard::JVSIOBoard;

  HandlerResponse HandleCommand(JVSIOCommand cmd, FrameContext ctx) override;
};

}  // namespace Triforce
