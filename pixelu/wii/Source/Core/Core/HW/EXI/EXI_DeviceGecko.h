// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "Common/CommonTypes.h"
#include "Core/HW/EXI/EXI_Device.h"

namespace sf
{
class TcpSocket;
}

namespace ExpansionInterface
{
class GeckoListener;

// The USB Gecko debugging adapter. The PC side is a TCP connection: Dolphin listens on port
// 55020 (or one of the next ten if it is taken) and bridges the first connected client to the
// adapter's FIFOs. Every access is a bidirectional immediate transfer whose top nibble is the
// command.
class CEXIGecko : public IEXIDevice
{
public:
  explicit CEXIGecko(Core::System& system);
  ~CEXIGecko() override;

  CEXIGecko(const CEXIGecko&) = delete;
  CEXIGecko& operator=(const CEXIGecko&) = delete;

  bool IsPresent() const override { return true; }
  void ImmReadWrite(u32& data, u32 size) override;

private:
  void AdoptWaitingClient();
  void StopPump();
  void Pump();

  std::shared_ptr<GeckoListener> m_listener;

  std::unique_ptr<sf::TcpSocket> m_client;
  std::thread m_pump_thread;
  std::atomic<bool> m_pump_running{false};

  // Both FIFOs are shared with the pump thread.
  std::mutex m_fifo_lock;
  std::deque<u8> m_to_pc;
  std::deque<u8> m_from_pc;
};
}  // namespace ExpansionInterface
