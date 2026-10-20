// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/SI/SI_DeviceGBA.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

namespace SerialInterface
{
namespace
{
constexpr unsigned short DATA_PORT = 0xd6ba;   // "dolphin gba"
constexpr unsigned short CLOCK_PORT = 0xc10c;  // "clock"
constexpr std::size_t MAX_PACKET = 5;

using SocketPtr = std::unique_ptr<sf::TcpSocket>;

// Accepts incoming emulator connections on a background thread and hands
// them out, oldest first, to whichever link port asks next.
class ConnectionWaiter
{
public:
  void EnsureRunning()
  {
    if (m_thread.joinable())
      return;
    m_running = true;
    m_thread = std::thread([this] { Run(); });
  }

  void Stop()
  {
    m_running = false;
    if (m_thread.joinable())
      m_thread.join();
  }

  SocketPtr TakeDataSocket() { return Take(m_data_sockets); }
  SocketPtr TakeClockSocket() { return Take(m_clock_sockets); }

private:
  SocketPtr Take(std::deque<SocketPtr>& queue)
  {
    std::lock_guard guard(m_lock);
    if (queue.empty())
      return nullptr;
    SocketPtr socket = std::move(queue.front());
    queue.pop_front();
    return socket;
  }

  void Run()
  {
    Common::SetCurrentThreadName("GBA Connection Waiter");

    sf::TcpListener data_listener;
    sf::TcpListener clock_listener;
    if (data_listener.listen(DATA_PORT) != sf::Socket::Status::Done)
      return;
    if (clock_listener.listen(CLOCK_PORT) != sf::Socket::Status::Done)
      return;
    data_listener.setBlocking(false);
    clock_listener.setBlocking(false);

    auto candidate = std::make_unique<sf::TcpSocket>();
    while (m_running)
    {
      // A socket that got accepted is queued and replaced by a fresh one.
      if (data_listener.accept(*candidate) == sf::Socket::Status::Done)
      {
        std::lock_guard guard(m_lock);
        m_data_sockets.push_back(std::move(candidate));
        candidate = std::make_unique<sf::TcpSocket>();
      }
      if (clock_listener.accept(*candidate) == sf::Socket::Status::Done)
      {
        std::lock_guard guard(m_lock);
        m_clock_sockets.push_back(std::move(candidate));
        candidate = std::make_unique<sf::TcpSocket>();
      }
      Common::SleepCurrentThread(1);
    }
  }

  std::thread m_thread;
  std::atomic<bool> m_running{false};
  std::mutex m_lock;
  std::deque<SocketPtr> m_data_sockets;
  std::deque<SocketPtr> m_clock_sockets;
};

ConnectionWaiter s_waiter;

// Reply size the GBA sends for each command.
u8 ReplyLength(EBufferCommands command)
{
  switch (command)
  {
  case EBufferCommands::CMD_RESET:
  case EBufferCommands::CMD_STATUS:
    return 3;
  case EBufferCommands::CMD_READ_GBA:
    return 5;
  default:
    return 1;
  }
}
}  // namespace

void GBAConnectionWaiter_Shutdown()
{
  s_waiter.Stop();
}

GBASockServer::GBASockServer()
{
  s_waiter.EnsureRunning();
}

GBASockServer::~GBASockServer()
{
  Disconnect();
}

void GBASockServer::Disconnect()
{
  if (m_data_socket)
  {
    m_data_socket->disconnect();
    m_data_socket.reset();
  }
  if (m_clock_socket)
  {
    m_clock_socket->disconnect();
    m_clock_socket.reset();
  }
  m_last_clock_ticks = 0;
  m_peer_answered = false;
}

void GBASockServer::ClockSync(Core::System& system)
{
  if (!m_clock_socket)
  {
    m_clock_socket = s_waiter.TakeClockSocket();
    if (!m_clock_socket)
      return;
  }

  const u64 now = system.GetCoreTiming().GetTicks();
  const u64 tps = system.GetSystemTimers().GetTicksPerSecond();

  // The first message claims one frame (1/60 s) has passed. Elapsed time is
  // sent in units of 2^-24 seconds, as a big-endian 32-bit value.
  const u32 elapsed =
      m_last_clock_ticks == 0 ? static_cast<u32>(tps / 60) : static_cast<u32>(now - m_last_clock_ticks);
  const u32 scaled = static_cast<u32>(u64{elapsed} * 16777216 / tps);
  m_last_clock_ticks = now;

  const std::array<u8, 4> message = {static_cast<u8>(scaled >> 24), static_cast<u8>(scaled >> 16),
                                     static_cast<u8>(scaled >> 8), static_cast<u8>(scaled)};
  if (m_clock_socket->send(message.data(), message.size()) == sf::Socket::Status::Disconnected)
  {
    m_clock_socket->disconnect();
    m_clock_socket.reset();
  }
}

bool GBASockServer::Connect()
{
  if (!m_data_socket)
  {
    m_data_socket = s_waiter.TakeDataSocket();
    if (m_data_socket)
      m_data_socket->setBlocking(false);
  }
  return m_data_socket != nullptr;
}

bool GBASockServer::IsConnected()
{
  return m_data_socket != nullptr;
}

void GBASockServer::Send(const u8* si_buffer)
{
  if (!Connect())
    return;

  // Only a write carries data; every other command is a single byte.
  const bool is_write = static_cast<EBufferCommands>(si_buffer[0]) == EBufferCommands::CMD_WRITE_GBA;
  std::array<u8, MAX_PACKET> packet;
  std::copy_n(si_buffer, packet.size(), packet.begin());
  const std::size_t length = is_write ? packet.size() : 1;

  if (m_data_socket->send(packet.data(), length) == sf::Socket::Status::Disconnected)
    Disconnect();
}

int GBASockServer::Receive(u8* si_buffer, u8 bytes)
{
  if (!m_data_socket)
    return 0;

  // Once the peer is known to answer, give it up to a second.
  if (m_peer_answered)
  {
    sf::SocketSelector selector;
    selector.add(*m_data_socket);
    (void)selector.wait(sf::milliseconds(1000));
  }

  std::array<u8, MAX_PACKET> reply{};
  std::size_t received = 0;
  const auto status = m_data_socket->receive(reply.data(), bytes, received);
  if (status == sf::Socket::Status::Disconnected)
  {
    Disconnect();
    return 0;
  }
  if (status == sf::Socket::Status::NotReady || received == 0)
  {
    m_peer_answered = false;
    return 0;
  }

  m_peer_answered = true;
  std::copy(reply.begin(), reply.end(), si_buffer);
  return static_cast<int>(std::min(received, reply.size()));
}

void GBASockServer::Flush()
{
  if (!m_data_socket)
    return;

  // Drop any late replies to commands that already timed out.
  u8 discard;
  std::size_t received = 0;
  do
  {
    if (m_data_socket->receive(&discard, 1, received) != sf::Socket::Status::Done)
      break;
  } while (received != 0);
}

CSIDevice_GBA::CSIDevice_GBA(Core::System& system, SIDevices device, int device_number)
    : ISIDevice(system, device, device_number)
{
}

int CSIDevice_GBA::RunBuffer(u8* buffer, int)
{
  auto& timing = m_system.GetCoreTiming();

  if (m_phase == Phase::Idle)
  {
    m_link.ClockSync(m_system);
    if (!m_link.Connect())
      return -1;

    m_link.Flush();
    m_link.Send(buffer);
    m_pending_command = static_cast<EBufferCommands>(buffer[0]);
    m_sent_at = timing.GetTicks();
    m_phase = Phase::InFlight;
    return 0;
  }

  if (m_phase == Phase::InFlight)
  {
    const int elapsed = static_cast<int>(timing.GetTicks() - m_sent_at);
    if (elapsed < SIDevice_GetGBATransferTime(m_system.GetSystemTimers(), m_pending_command))
      return 0;
    m_phase = Phase::AwaitReply;
  }

  // Phase::AwaitReply
  m_phase = Phase::Idle;
  const int received = m_link.Receive(buffer, ReplyLength(m_pending_command));
  if (received == 0)
    return -1;
  DEBUG_LOG_FMT(SERIALINTERFACE, "GBA {}: reply to {:#04x}, {} bytes", m_device_number,
                static_cast<u8>(m_pending_command), received);
  return received;
}

int CSIDevice_GBA::TransferInterval()
{
  return SIDevice_GetGBATransferTime(m_system.GetSystemTimers(), m_pending_command);
}

DataResponse CSIDevice_GBA::GetData(u32&, u32&)
{
  return DataResponse::NoData;
}

void CSIDevice_GBA::SendCommand(u32, u8)
{
}
}  // namespace SerialInterface
