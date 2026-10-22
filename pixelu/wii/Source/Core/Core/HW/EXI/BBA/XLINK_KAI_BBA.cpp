// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/HW/EXI/EXI_DeviceEthernet.h"

#include "SFML/Network/IpAddress.hpp"
#include "VideoCommon/OnScreenDisplay.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

// The broadband adapter as a client of XLink Kai's emulator integration protocol
// (https://www.teamxlink.co.uk/wiki/Emulator_Integration_Protocol): text commands and Ethernet
// frames prefixed with "e;e;" in UDP datagrams to the XLink Kai client. The product's name is
// written "XLink Kai".

namespace ExpansionInterface
{
namespace
{
constexpr std::string_view FRAME_PREFIX = "e;e;";
// Commands are sent from a zeroed buffer of this size.
constexpr std::size_t COMMAND_BUFFER_SIZE = 255;
// The keepalive is answered by echoing its first bytes ("keepalive;").
constexpr std::size_t KEEPALIVE_REPLY_SIZE = 10;

// The text after a "name;" prefix, or nothing if the message is that short.
std::string TextAfter(const std::string& message, std::size_t prefix_length)
{
  return message.size() > prefix_length ? message.substr(prefix_length) : std::string();
}
}  // namespace

bool CEXIETHERNET::XLinkNetworkInterface::SendDatagram(const void* data, std::size_t size)
{
  return m_socket.send(data, size, m_peer, static_cast<unsigned short>(m_dest_port)) ==
         sf::Socket::Status::Done;
}

void CEXIETHERNET::XLinkNetworkInterface::SendCommand(std::string_view command)
{
  std::array<u8, COMMAND_BUFFER_SIZE> buffer{};
  const u32 size = static_cast<u32>(std::min(command.size(), buffer.size()));
  std::copy_n(command.begin(), size, buffer.begin());
  DEBUG_LOG_FMT(SP1, "SendCommandPayload {:x}\n{}", size, ArrayToString(buffer.data(), size, 0x10));
  if (!SendDatagram(buffer.data(), size))
    ERROR_LOG_FMT(SP1, "Failed to send '{}' to the XLink Kai client", command.substr(0, 16));
}

bool CEXIETHERNET::XLinkNetworkInterface::Activate()
{
  if (IsActivated())
    return true;

  if (m_socket.bind(sf::Socket::AnyPort) != sf::Socket::Status::Done)
  {
    ERROR_LOG_FMT(SP1, "Couldn't open XLink Kai UDP socket, unable to initialize BBA");
    return false;
  }
  m_peer = sf::IpAddress::resolve(m_dest_ip).value_or(sf::IpAddress::Any);

  // connect;locally_unique_name;emulator_name;optional_padding
  SendCommand("connect;" + m_client_identifier +
              ";dolphin;000000000000000000000000000000000000000000");

  INFO_LOG_FMT(SP1, "BBA initialized.");
  return RecvInit();
}

void CEXIETHERNET::XLinkNetworkInterface::Deactivate()
{
  if (!IsActivated())
    return;

  // disconnect;optional_locally_unique_name;optional_padding
  SendCommand("disconnect;" + m_client_identifier + ";0000000000000000000000000000000000000000000");
  NOTICE_LOG_FMT(SP1, "XLink Kai BBA deactivated");

  m_link_up = false;

  // The reader may be blocked in receive; stop it and let it finish on its own.
  m_socket.setBlocking(false);
  m_receiving.Clear();
  m_stop_reading.Set();
  m_read_thread.detach();

  m_socket.unbind();
}

bool CEXIETHERNET::XLinkNetworkInterface::IsActivated()
{
  return m_socket.getLocalPort() > 0;
}

bool CEXIETHERNET::XLinkNetworkInterface::SendFrame(const u8* frame, u32 size)
{
  if (!m_link_up)
  {
    // Frames are dropped until the client says it is connected. That is not an error for the
    // adapter, but the user hears about it once.
    if (!m_told_user_not_connected)
    {
      OSD::AddMessage("XLink Kai BBA not connected", 30000);
      m_told_user_not_connected = true;
    }
    return true;
  }

  std::copy(FRAME_PREFIX.begin(), FRAME_PREFIX.end(), m_out.begin());
  std::memcpy(m_out.data() + FRAME_PREFIX.size(), frame, size);
  const std::size_t datagram_size = FRAME_PREFIX.size() + size;

  if (!SendDatagram(m_out.data(), datagram_size))
  {
    ERROR_LOG_FMT(SP1, "SendFrame(): expected to write {} bytes, but failed, errno {}",
                  datagram_size, errno);
    return false;
  }

  m_eth_ref->SendComplete();
  return true;
}

void CEXIETHERNET::XLinkNetworkInterface::HandleControlMessage(std::size_t size)
{
  const std::string message(m_in.data(), size);
  INFO_LOG_FMT(SP1, "Received XLink Kai control data: {}", message);

  if (message.starts_with("connected"))
  {
    // connected;identifier;
    NOTICE_LOG_FMT(SP1, "XLink Kai BBA connected");
    OSD::AddMessage("XLink Kai BBA connected", 4500);
    m_link_up = true;
    if (m_chat_osd_enabled)
      SendCommand("setting;chat;true;");
  }
  else if (message.starts_with("disconnected"))
  {
    // disconnected;optional_identifier;optional_message;
    NOTICE_LOG_FMT(SP1, "XLink Kai BBA disconnected");
    OSD::AddMessage("XLink Kai BBA disconnected", 15000);
    m_link_up = false;
    m_socket.setBlocking(false);
    m_receiving.Clear();
    m_stop_reading.Set();
    m_socket.unbind();
  }
  else if (message.starts_with("keepalive"))
  {
    DEBUG_LOG_FMT(SP1, "XLink Kai BBA keepalive");
    if (!SendDatagram(m_in.data(), KEEPALIVE_REPLY_SIZE))
      ERROR_LOG_FMT(SP1, "Failed to answer an XLink Kai keepalive");
  }
  else if (message.starts_with("message"))
  {
    // message;text;
    std::string text = TextAfter(message, 8);
    NOTICE_LOG_FMT(SP1, "XLink Kai message: {}", text);
    OSD::AddMessage(std::move(text), 15000);
  }
  else if (message.starts_with("chat"))
  {
    // chat;text;
    std::string text = TextAfter(message, 5);
    NOTICE_LOG_FMT(SP1, "XLink Kai chat: {}", text);
    OSD::AddMessage(std::move(text), 5000);
  }
  else if (message.starts_with("directmessage"))
  {
    // directmessage;text;
    std::string text = TextAfter(message, 14);
    NOTICE_LOG_FMT(SP1, "XLink Kai direct message: {}", text);
    OSD::AddMessage(std::move(text), 5000);
  }
  // Anything else is ignored.
}

void CEXIETHERNET::XLinkNetworkInterface::ReadLoop()
{
  std::optional<sf::IpAddress> sender;
  unsigned short sender_port = 0;

  while (!m_stop_reading.IsSet() && IsActivated())
  {
    std::size_t size = 0;
    const sf::Socket::Status status =
        m_socket.receive(m_in.data(), m_in.size(), size, sender, sender_port);
    if (status != sf::Socket::Status::Done && m_link_up)
      ERROR_LOG_FMT(SP1, "Failed to read from BBA, err={}", size);
    if (size == 0)
      continue;

    // Frames carry the "e;e;" prefix; only its second half is checked.
    const bool is_frame = m_in[2] == 'e' && m_in[3] == ';';
    if (!is_frame)
    {
      HandleControlMessage(size);
      if (m_stop_reading.IsSet())
        break;
      continue;
    }

    // Too short or too large frames are dropped.
    if (size < FRAME_PREFIX.size() || size - FRAME_PREFIX.size() >= BBA_RECV_SIZE)
      continue;
    if (m_receiving.IsSet())
    {
      m_eth_ref->ReceiveFrame(reinterpret_cast<const u8*>(m_in.data()) + FRAME_PREFIX.size(),
                              static_cast<u32>(size - FRAME_PREFIX.size()));
    }
  }
}

bool CEXIETHERNET::XLinkNetworkInterface::RecvInit()
{
  m_read_thread = std::thread(&XLinkNetworkInterface::ReadLoop, this);
  return true;
}

void CEXIETHERNET::XLinkNetworkInterface::RecvStart()
{
  m_receiving.Set();
}

void CEXIETHERNET::XLinkNetworkInterface::RecvStop()
{
  m_receiving.Clear();
}
}  // namespace ExpansionInterface
