// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The network board with its 68000 emulated, running the program the main
// board loads into it. Its 68000 sees:
//   0x0xxxx  its 64 KB of RAM (the main board's net RAM window)
//   0x4xxxx  control registers (IRQ acknowledges, bank select, LEDs ...)
//   0x8xxxx  the 64 KB of shared RAM, the main board's window on the link
//   0xcxxxx  the link controller's registers (also seen by the main board)
// Sending and receiving go over TCP round the ring of linked machines.

#ifndef INCLUDED_NETBOARD_H
#define INCLUDED_NETBOARD_H

#include "Types.h"
#include "CPU/Bus.h"
#include "CPU/68K/68K.h"
#include "OSD/Thread.h"
#include <memory>
#include <string>
#include "INetBoard.h"
#include "TCPSend.h"
#include "TCPSendAsync.h"
#include "TCPReceive.h"

class CNetBoard : public IBus, public INetBoard
{
public:
  CNetBoard(const Util::Config::Node &config);
  ~CNetBoard(void);

  // IBus: the 68000's view
  UINT8 Read8(UINT32 addr);
  UINT16 Read16(UINT32 addr);
  UINT32 Read32(UINT32 addr);
  void Write8(UINT32 addr, UINT8 data);
  void Write16(UINT32 addr, UINT16 data);
  void Write32(UINT32 addr, UINT32 data);

  // INetBoard: the main board's view
  void SaveState(CBlockFile *state);
  void LoadState(CBlockFile *state);
  void RunFrame(void);
  void Reset(void);
  bool IsAttached(void);
  bool IsRunning(void);
  Result Init(UINT8 *netRAMPtr, UINT8 *netBufferPtr);
  void GetGame(const Game &game);
  UINT8 ReadCommRAM8(unsigned addr);
  UINT16 ReadCommRAM16(unsigned addr);
  UINT32 ReadCommRAM32(unsigned addr);
  void WriteCommRAM8(unsigned addr, UINT8 data);
  void WriteCommRAM16(unsigned addr, UINT16 data);
  void WriteCommRAM32(unsigned addr, UINT32 data);
  UINT16 ReadIORegister(unsigned reg);
  void WriteIORegister(unsigned reg, UINT16 data);

  // The 68000's context (for the debugger).
  M68KCtx *GetM68K(void);

private:
  void IoWrite8(unsigned reg, UINT8 data);
  void ControlWrite(unsigned reg, bool wide);

  const Util::Config::Node &m_config;
  M68KCtx m_cpu;
  Game m_game;
  bool m_attached = false;

  UINT8 *m_ram = nullptr;         // 64 KB, the main board's net RAM
  UINT8 *m_shared = nullptr;      // 64 KB at the start of the net buffer
  UINT8 *m_io = nullptr;          // link registers, right after m_shared
  std::unique_ptr<UINT8[]> m_control;   // 256 bytes of control registers

  // set a byte at a time by the 68000 (low byte first)
  UINT16 m_bank = 0;
  UINT16 m_rxOffset = 0;
  UINT16 m_rxSize = 0;
  UINT16 m_txOffset = 0;
  UINT16 m_txSize = 0;

  UINT16 m_portIn = 0;
  UINT16 m_portOut = 0;
  std::string m_addressOut;
  std::unique_ptr<TCPSend> m_send;
  std::unique_ptr<TCPReceive> m_receive;
};

// Kept for callers; the callbacks are stored but not used.
void Net_SetCB(int (*Run68k)(int cycles), void (*Int68k)(int irq));

#endif // INCLUDED_NETBOARD_H
