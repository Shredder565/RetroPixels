// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/EXI_DeviceGecko.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <SFML/Network.hpp>
#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Core.h"

namespace ExpansionInterface
{
// Accepts PC connections for every Gecko device. One listener thread serves all adapters; it
// lives as long as some adapter holds a reference to it.
class GeckoListener
{
public:
  GeckoListener() : m_thread(&GeckoListener::Run, this) {}
  ~GeckoListener()
  {
    m_running = false;
    m_thread.join();
  }

  GeckoListener(const GeckoListener&) = delete;
  GeckoListener& operator=(const GeckoListener&) = delete;

  static std::shared_ptr<GeckoListener> Acquire()
  {
    static std::mutex s_lock;
    static std::weak_ptr<GeckoListener> s_instance;
    std::lock_guard guard(s_lock);
    std::shared_ptr<GeckoListener> listener = s_instance.lock();
    if (!listener)
    {
      listener = std::make_shared<GeckoListener>();
      s_instance = listener;
    }
    return listener;
  }

  // Hands out the oldest connection nobody has taken yet.
  std::unique_ptr<sf::TcpSocket> TakeConnection()
  {
    std::lock_guard guard(m_lock);
    if (m_pending.empty())
      return nullptr;
    std::unique_ptr<sf::TcpSocket> socket = std::move(m_pending.front());
    m_pending.pop();
    return socket;
  }

private:
  static constexpr u16 FIRST_PORT = 0xd6ec;  // 55020
  static constexpr int EXTRA_PORTS = 10;

  void Run()
  {
    Common::SetCurrentThreadName("Gecko Connection Waiter");

    sf::TcpListener listener;
    u16 port = FIRST_PORT;
    bool bound = false;
    for (int attempt = 0; attempt <= EXTRA_PORTS; ++attempt, ++port)
    {
      if (listener.listen(port) == sf::Socket::Status::Done)
      {
        bound = true;
        break;
      }
    }
    if (!bound)
      return;

    Core::DisplayMessage(fmt::format("USBGecko: Listening on TCP port {}", port), 5000);
    listener.setBlocking(false);

    auto candidate = std::make_unique<sf::TcpSocket>();
    while (m_running)
    {
      if (listener.accept(*candidate) == sf::Socket::Status::Done)
      {
        std::lock_guard guard(m_lock);
        m_pending.push(std::move(candidate));
        candidate = std::make_unique<sf::TcpSocket>();
      }
      Common::SleepCurrentThread(1);
    }
  }

  std::atomic<bool> m_running{true};
  std::mutex m_lock;
  std::queue<std::unique_ptr<sf::TcpSocket>> m_pending;
  std::thread m_thread;
};

namespace
{
// Commands, in the top nibble of the transfer word.
constexpr u32 GECKO_LED_OFF = 0x7;
constexpr u32 GECKO_LED_ON = 0x8;
constexpr u32 GECKO_IDENTIFY = 0x9;
constexpr u32 GECKO_RECEIVE = 0xa;
constexpr u32 GECKO_SEND = 0xb;
constexpr u32 GECKO_CAN_SEND = 0xc;
constexpr u32 GECKO_CAN_RECEIVE = 0xd;

constexpr u32 GECKO_ID = 0x04700000;
constexpr u32 RECEIVED_BYTE = 0x08000000;
constexpr u32 ACCEPTED = 0x04000000;
}  // namespace

CEXIGecko::CEXIGecko(Core::System& system)
    : IEXIDevice(system), m_listener(GeckoListener::Acquire())
{
}

CEXIGecko::~CEXIGecko()
{
  StopPump();
}

void CEXIGecko::StopPump()
{
  m_pump_running = false;
  if (m_pump_thread.joinable())
    m_pump_thread.join();
}

void CEXIGecko::AdoptWaitingClient()
{
  std::unique_ptr<sf::TcpSocket> socket = m_listener->TakeConnection();
  if (!socket)
    return;

  // A new PC connection replaces the old one and starts with empty FIFOs.
  StopPump();
  m_client = std::move(socket);
  m_to_pc.clear();
  m_from_pc.clear();
  m_pump_running = true;
  m_pump_thread = std::thread(&CEXIGecko::Pump, this);
}

void CEXIGecko::Pump()
{
  Common::SetCurrentThreadName("Gecko Client");
  m_client->setBlocking(false);

  std::array<char, 128> chunk;
  std::vector<char> outgoing;
  while (m_pump_running)
  {
    bool busy = false;
    {
      std::lock_guard guard(m_fifo_lock);

      std::size_t received = 0;
      if (m_client->receive(chunk.data(), chunk.size(), received) ==
          sf::Socket::Status::Disconnected)
      {
        m_pump_running = false;
      }
      if (received != 0)
      {
        m_from_pc.insert(m_from_pc.end(), chunk.begin(), chunk.begin() + received);
        busy = true;
      }

      if (!m_to_pc.empty())
      {
        outgoing.assign(m_to_pc.begin(), m_to_pc.end());
        m_to_pc.clear();
        if (m_client->send(outgoing.data(), outgoing.size()) == sf::Socket::Status::Disconnected)
          m_pump_running = false;
        busy = true;
      }
    }

    if (!busy)
      Common::YieldCPU();
  }

  m_client->disconnect();
}

void CEXIGecko::ImmReadWrite(u32& data, u32)
{
  // Pick up a PC connection while there is none (or the last one was closed).
  if (!m_client || m_client->getLocalPort() == 0)
    AdoptWaitingClient();

  switch (data >> 28)
  {
  case GECKO_LED_OFF:
    Core::DisplayMessage("USBGecko: No LEDs for you!", 3000);
    break;

  case GECKO_LED_ON:
    Core::DisplayMessage("USBGecko: A piercing blue light is now shining in your general direction",
                         3000);
    break;

  case GECKO_IDENTIFY:
    data = GECKO_ID;
    break;

  case GECKO_RECEIVE:
  {
    // PC to console: the byte comes back in bits 16-23 when one was waiting.
    std::lock_guard guard(m_fifo_lock);
    if (!m_from_pc.empty())
    {
      data = RECEIVED_BYTE | (u32{m_from_pc.front()} << 16);
      m_from_pc.pop_front();
    }
    break;
  }

  case GECKO_SEND:
  {
    // Console to PC: the byte is in bits 20-27. The FIFO never fills.
    std::lock_guard guard(m_fifo_lock);
    m_to_pc.push_back(static_cast<u8>(data >> 20));
    data = ACCEPTED;
    break;
  }

  case GECKO_CAN_SEND:
    data = ACCEPTED;
    break;

  case GECKO_CAN_RECEIVE:
  {
    std::lock_guard guard(m_fifo_lock);
    data = m_from_pc.empty() ? 0 : ACCEPTED;
    break;
  }

  default:
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "Unknown USBGecko command {:x}", data);
    break;
  }
}
}  // namespace ExpansionInterface
