// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Model 3 drive boards: the Z80 boards behind force feedback (wheel and
// joystick), the ski pad's rumble and the billboard displays. CDriveBoard is
// the common part (a Z80 with ROM, 8 KB of RAM, a CTC for its NMI timer and
// a byte latch each way to the PowerPC) and, used on its own, the stand-in
// for games without a drive board (never attached).
//
// Attached and disabled: a board is attached once, by Init() with a ROM.
// Emulation runs only while it is attached and not disabled. Halt() stops
// it for good when something goes wrong (an incompatible save state, say);
// only Reset() clears that again, and only for an attached board.

#ifndef INCLUDED_DRIVEBOARD_H
#define INCLUDED_DRIVEBOARD_H

#include "CPU/Bus.h"
#include "CPU/Z80/Z80.h"
#include "Inputs/Inputs.h"
#include "OSD/Outputs.h"
#include "Util/NewConfig.h"
#include "Game.h"
#include "Z80CTC.h"

class CDriveBoard : public IBus
{
public:
  virtual Game::DriveBoardType GetType(void) const;
  // Whether the game has this board (and it should be emulated).
  virtual bool IsAttached(void) const;
  // Whether it is simulated rather than run on its Z80.
  virtual bool IsSimulated(void) const;

  virtual void GetDIPSwitches(UINT8 &dip1, UINT8 &dip2) const;
  virtual void GetDIPSwitches(UINT8 &dip1) const;
  virtual void SetDIPSwitches(UINT8 dip1, UINT8 dip2);
  virtual void SetDIPSwitches(UINT8 dip1);

  // Force feedback strength 1-8 (DIP switch 1, bits 2-4, inverted).
  virtual unsigned GetForceFeedbackStrength(void) const;
  virtual void SetForceFeedbackStrength(unsigned strength);

  virtual CZ80 *GetZ80(void);

  virtual void SaveState(CBlockFile *SaveState);
  virtual void LoadState(CBlockFile *SaveState);

  // Attaches the board with its Z80 program ROM (the data may be loaded
  // later). Call once, before anything else.
  virtual Result Init(const UINT8 *romPtr);
  // A board that is not attached (with a blank ROM).
  Result Init(void);

  virtual void AttachInputs(CInputs *inputs, unsigned gameInputFlags);
  virtual void AttachOutputs(COutputs *outputs);

  virtual void Reset(void);

  // The PowerPC's side: the board's reply, and a command for it.
  virtual UINT8 Read(void);
  virtual void Write(UINT8 data);

  // One video frame of the board's time.
  virtual void RunFrame(void);

  // The configuration is held by reference (it changes while running).
  CDriveBoard(const Util::Config::Node &config);
  virtual ~CDriveBoard(void);

  // Z80 bus
  virtual UINT8 Read8(UINT32 addr);
  virtual UINT8 IORead8(UINT32 portNum);
  virtual void Write8(UINT32 addr, UINT8 data);
  virtual void IOWrite8(UINT32 portNum, UINT8 data);

protected:
  // Stops emulation (boards override it to stop their effects first).
  virtual void Halt(void);
  // Not attached, or disabled.
  bool Inactive(void) const;
  // Z80 cycles in one video frame (57.524 Hz).
  int CyclesPerFrame() const;

  const Util::Config::Node &m_config;
  bool m_present = false;
  bool m_halted = true;           // until a Reset() of an attached board
  bool m_simulate = false;

  // Saved state (do not change these types)
  bool m_ready = false;       // the board has finished starting up
  bool m_intEnabled = false;   // raise the Z80's INT every time slice
  bool m_nmiEnabled = false;          // the CTC's channel 1 timer gives NMIs
  UINT8 m_command = 0;              // last byte from the PowerPC
  UINT8 m_reply = 0;                // byte for the PowerPC
  UINT8 m_dipA = 0;
  UINT8 m_dipB = 0;
  int m_nmiCountdown = 0;           // Z80 cycles to the next NMI

  // Simulation (boards that answer the PowerPC without running the Z80)
  UINT8 m_startupFrames = 0;            // frames since the start-up sequence began
  UINT8 m_simStatus = 0;
  UINT8 m_simMode = 0;
  UINT8 m_simSelect = 0;
  UINT8 m_simCenter = 0;
  UINT8 m_simEcho = 0;

  const UINT8 *m_program = nullptr;     // Z80 program (0x0000-0x8FFF)
  UINT8 *m_work = nullptr;           // 8 KB at 0xE000
  const UINT8 *m_blankProgram = nullptr;
  CZ80 m_cpu;
  float m_clockMHz = 8.0f;          // MHz
  Z80CTC m_ctc;

  CInputs *m_in = nullptr;
  unsigned m_gameInputs = 0;
  COutputs *m_out = nullptr;

private:
  // The NMI period from the CTC's channel 1.
  int NmiInterval();
};

#endif // INCLUDED_DRIVEBOARD_H
