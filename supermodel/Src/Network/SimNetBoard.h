// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// A stand-in for the network board that skips emulating its 68000: it
// answers the main board's start-up the way the real board's program would,
// then each frame passes every machine's slice of the shared RAM round the
// ring of linked machines over TCP (TCPSend to the next, TCPReceive from the
// previous).

#ifndef INCLUDED_SIMNETBOARD_H
#define INCLUDED_SIMNETBOARD_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include "TCPSend.h"
#include "TCPReceive.h"
#include "INetBoard.h"

// Where the simulated board is in its start-up.
enum class State
{
  start,    // not begun
  init,     // waiting for the main board to set it up
  testing,  // finding the other machines
  ready,    // linked: data is exchanged every frame
  error     // given up
};

// The two ways the games drive the board.
enum class GameType
{
  unknown,
  one,      // Daytona 2, Harley, Scud Race, Sega Rally 2, Ski Champ, Spikeout
  two       // Le Mans 24, Virtual-On, Dirt Devils
};

class CSimNetBoard : public INetBoard
{
public:
  CSimNetBoard(const Util::Config::Node &config);
  ~CSimNetBoard(void);

  void SaveState(CBlockFile *state);
  void LoadState(CBlockFile *state);

  Result Init(uint8_t *netRAMPtr, uint8_t *netBufferPtr);
  void RunFrame(void);
  void Reset(void);

  bool IsAttached(void);
  bool IsRunning(void);

  void GetGame(const Game &game);

  uint8_t ReadCommRAM8(unsigned addr);
  uint16_t ReadCommRAM16(unsigned addr);
  uint32_t ReadCommRAM32(unsigned addr);
  void WriteCommRAM8(unsigned addr, uint8_t data);
  void WriteCommRAM16(unsigned addr, uint16_t data);
  void WriteCommRAM32(unsigned addr, uint32_t data);

  uint16_t ReadIORegister(unsigned reg);
  void WriteIORegister(unsigned reg, uint16_t data);

private:
  // How a step of the link-up went.
  enum class Step { done, again, failed };

  bool Is(const char *name) const;
  uint16_t Ram(unsigned offset) const;
  void SetShared(unsigned offset, uint16_t value);
  bool Take(void *dst, size_t bytes);
  Step CheckBuild(bool master);
  Step CountMachines(bool master, size_t bytes, uint8_t *index, uint8_t *count, bool relay);
  bool AloneByMistake(unsigned others) const;
  void LinkType1();
  void LinkType2();
  void Exchange();
  void Connect();

  const Util::Config::Node &m_config;

  uint8_t *m_ram = nullptr;       // the board's own RAM (set by the main board)
  uint8_t *m_buffer = nullptr;    // two 64 KB banks of shared RAM
  uint8_t *m_local = nullptr;     // the bank the board is using this frame
  uint8_t *m_external = nullptr;  // the bank the main board sees

  uint16_t m_portIn = 0;
  uint16_t m_portOut = 0;
  std::string m_addressOut;
  std::unique_ptr<TCPSend> m_send;
  std::unique_ptr<TCPReceive> m_receive;
  std::thread m_connector;
  std::atomic_bool m_stop{ false };
  std::atomic_bool m_linked{ false };

  Game m_game;
  GameType m_type = GameType::unknown;
  State m_state = State::start;

  uint8_t m_machines = 0;         // all machines in the ring, this one included
  uint16_t m_frame = 0;
  uint16_t m_slice = 0;           // bytes each machine sends a frame

  bool m_attached = false;
  bool m_running = false;
  bool m_bank = false;

  uint16_t m_reg00 = 0;           // IRQ 2 acknowledge
  uint16_t m_reg88 = 0;           // status words
  uint16_t m_reg8a = 0;
};

#endif // INCLUDED_SIMNETBOARD_H
