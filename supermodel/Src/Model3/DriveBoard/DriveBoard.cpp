// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The common drive board (see DriveBoard.h).
//
// Z80 memory: 0000-8FFF ROM, E000-FFFF RAM (8 KB), the rest reads 0xFF.
// Ports 10-13 are the CTC; channel 1's timer gives the NMI. F0/F1 are the
// watchdog. The boards add their own ports.

#include "DriveBoard.h"
#include "Supermodel.h"

#include <algorithm>
#include <cstring>

namespace
{
  constexpr unsigned kROMSize = 0x9000;
  constexpr unsigned kRAMSize = 0x2000;
}

// bools are saved as single bytes
static_assert(sizeof(bool) == 1, "the save state stores bools as bytes");


/******************************************************************************
 Save states

 Block "DriveBoard.2": enabled (bool), then if enabled: simulated (bool),
 then if not simulated: RAM, initialized, NMI allowed, interrupts allowed,
 command, reply (bytes), NMI countdown (int), the CTC, and the Z80 (block
 "DriveBoard Z80").
******************************************************************************/

void CDriveBoard::SaveState(CBlockFile *SaveState)
{
  SaveState->NewBlock("DriveBoard.2", __FILE__);
  const bool enabled = !Inactive();
  SaveState->Write(enabled);
  if (!enabled)
    return;
  SaveState->Write(m_simulate);
  if (m_simulate)
    return;   // (no simulation state)
  SaveState->Write(m_work, kRAMSize);
  SaveState->Write(m_ready);
  SaveState->Write(m_nmiEnabled);
  SaveState->Write(m_intEnabled);
  SaveState->Write(m_command);
  SaveState->Write(m_reply);
  SaveState->Write(m_nmiCountdown);
  m_ctc.SaveState(SaveState);
  m_cpu.SaveState(SaveState, "DriveBoard Z80");
}

void CDriveBoard::LoadState(CBlockFile *SaveState)
{
  if (SaveState->FindBlock("DriveBoard.2") != Result::OKAY)
  {
    ErrorLog("Unable to load base drive board state. Save state file is corrupt.");
    Halt();
    return;
  }

  const bool enabledNow = !Inactive();
  bool wasEnabled = false;
  bool wasSimulated = false;
  SaveState->Read(wasEnabled);
  if (wasEnabled)
  {
    SaveState->Read(wasSimulated);
    if (!wasSimulated)
    {
      SaveState->Read(m_work, kRAMSize);
      SaveState->Read(m_ready);
      SaveState->Read(m_nmiEnabled);
      SaveState->Read(m_intEnabled);
      SaveState->Read(m_command);
      SaveState->Read(m_reply);
      SaveState->Read(m_nmiCountdown);
      m_ctc.LoadState(SaveState);
      m_cpu.LoadState(SaveState, "DriveBoard Z80");
    }
  }

  // A state saved in another mode (enabled or not, simulated or not) cannot
  // be resumed.
  if (wasEnabled != enabledNow || wasSimulated != m_simulate)
  {
    Halt();
    ErrorLog("Halting drive board emulation due to mismatch in active and restored states.");
  }
}


/******************************************************************************
 Configuration and state
******************************************************************************/

int CDriveBoard::CyclesPerFrame() const
{
  const int hz = (int)m_clockMHz * 1000000;
  return (int)(hz / 57.524);
}

Game::DriveBoardType CDriveBoard::GetType(void) const
{
  return Game::DRIVE_BOARD_NONE;
}

bool CDriveBoard::IsAttached(void) const
{
  return m_present;
}

bool CDriveBoard::IsSimulated(void) const
{
  return m_simulate;
}

bool CDriveBoard::Inactive(void) const
{
  return !m_present || m_halted;
}

void CDriveBoard::Halt(void)
{
  m_halted = true;
}

void CDriveBoard::GetDIPSwitches(UINT8 &dip1, UINT8 &dip2) const
{
  dip1 = m_dipA;
  dip2 = m_dipB;
}

void CDriveBoard::GetDIPSwitches(UINT8 &dip1) const
{
  dip1 = m_dipA;
}

void CDriveBoard::SetDIPSwitches(UINT8 dip1, UINT8 dip2)
{
  m_dipA = dip1;
  m_dipB = dip2;
}

void CDriveBoard::SetDIPSwitches(UINT8 dip1)
{
  m_dipA = dip1;
}

unsigned CDriveBoard::GetForceFeedbackStrength() const
{
  return (~(m_dipA >> 2) & 7) + 1;
}

void CDriveBoard::SetForceFeedbackStrength(unsigned strength)
{
  m_dipA = (UINT8)((m_dipA & 0xE3) | ((~(strength - 1) & 7) << 2));
}

CZ80 *CDriveBoard::GetZ80(void)
{
  return &m_cpu;
}

void CDriveBoard::AttachInputs(CInputs *inputs, unsigned gameInputFlags)
{
  m_in = inputs;
  m_gameInputs = gameInputFlags;
}

void CDriveBoard::AttachOutputs(COutputs *outputs)
{
  m_out = outputs;
}


/******************************************************************************
 PowerPC side
******************************************************************************/

UINT8 CDriveBoard::Read(void)
{
  return Inactive() ? 0xFF : m_reply;
}

void CDriveBoard::Write(UINT8 data)
{
  m_command = data;
}


/******************************************************************************
 Z80 bus
******************************************************************************/

UINT8 CDriveBoard::Read8(UINT32 addr)
{
  if (addr < kROMSize)
    return m_program[addr];
  if (addr >= 0xE000)
    return m_work[(addr - 0xE000) & (kRAMSize - 1)];
  return 0xFF;
}

void CDriveBoard::Write8(UINT32 addr, UINT8 data)
{
  if (addr >= 0xE000)
    m_work[(addr - 0xE000) & (kRAMSize - 1)] = data;
}

UINT8 CDriveBoard::IORead8(UINT32)
{
  return 0xFF;
}

int CDriveBoard::NmiInterval()
{
  const int hz = (int)m_clockMHz * 1000000;
  return hz / m_ctc.CalcFrequency(1, hz);   // (unsigned division, as before)
}

void CDriveBoard::IOWrite8(UINT32 portNum, UINT8 data)
{
  if (portNum >= 0x10 && portNum <= 0x13)
  {
    const UINT32 channel = portNum & 3;
    m_ctc.Write(channel, data);
    if (channel == 1)
    {
      m_nmiEnabled = m_ctc.TimerRunning(1);
      if (m_nmiEnabled)
        m_nmiCountdown = NmiInterval();
    }
  }
  // (F0, F1: watchdog)
}


/******************************************************************************
 Running
******************************************************************************/

void CDriveBoard::RunFrame(void)
{
  if (Inactive())
    return;

  // Run up to each NMI, and raise INT after every slice when enabled.
  int remaining = CyclesPerFrame();
  while (remaining > 0)
  {
    if (m_nmiEnabled)
    {
      const int ran = m_cpu.Run(std::min<int>(remaining, m_nmiCountdown));
      remaining -= ran;
      m_nmiCountdown -= ran;
    }
    else
      remaining -= m_cpu.Run(remaining);

    if (m_nmiEnabled && m_nmiCountdown <= 0)
    {
      m_cpu.TriggerNMI();
      m_nmiCountdown = NmiInterval();
    }
    if (m_intEnabled)
      m_cpu.SetINT(true);   // (the billboard needs it)
  }
}

Result CDriveBoard::Init(const UINT8 *romPtr)
{
  if (romPtr == nullptr)
    return ErrorLog("Internal error: no drive board ROM supplied.");
  m_program = romPtr;

  m_work = new (std::nothrow) UINT8[kRAMSize];
  if (m_work == nullptr)
    return ErrorLog("Insufficient memory for drive board (needs %1.1f MB).", (float)kRAMSize / (float)0x100000);
  std::memset(m_work, 0, kRAMSize);

  m_cpu.Init(this, nullptr);
  m_present = true;
  return Result::OKAY;
}

Result CDriveBoard::Init(void)
{
  // A blank ROM, so that nothing (the debugger, say) reads through a null
  // pointer.
  if (m_blankProgram == nullptr)
  {
    UINT8 *rom = new (std::nothrow) UINT8[kROMSize];
    if (rom == nullptr)
      return ErrorLog("Insufficient memory for drive board.");
    std::memset(rom, 0xFF, kROMSize);
    m_blankProgram = rom;
  }
  const Result result = Init(m_blankProgram);
  m_present = false;
  return result;
}

void CDriveBoard::Reset()
{
  m_ready = false;
  m_startupFrames = 0;
  m_simSelect = 0;
  m_simMode = 0;
  m_simCenter = 0x80;
  m_intEnabled = false;
  m_nmiEnabled = false;
  m_command = 0;
  m_reply = 0;
  m_cpu.Reset();   // also when not attached, for a valid Z80 state
  m_ctc.Reset();

  // (here rather than in Init(): the command line is read after that)
  SetForceFeedbackStrength(m_config["ForceFeedbackStrength"].ValueAsDefault<unsigned>(5));

  // The only place a board is enabled.
  if (m_present)
    m_halted = false;
}

CDriveBoard::CDriveBoard(const Util::Config::Node &config)
  : m_config(config)
{
}

CDriveBoard::~CDriveBoard(void)
{
  delete[] m_work;
  delete[] m_blankProgram;
  m_work = nullptr;
  m_blankProgram = nullptr;
}
