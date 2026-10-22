// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The Model 3 sound board: a 68000 with two SCSP sound chips (Sound/SCSP.h)
// and their RAM, program and sample ROMs. The PowerPC talks to it through
// the MIDI port; a Digital Sound Board (DSB.h), when the game has one,
// receives the same bytes and its music is mixed into the front channels.
// There is only one sound board (the 68000 core and the SCSPs are global).

#ifndef INCLUDED_SOUNDBOARD_H
#define INCLUDED_SOUNDBOARD_H

#include "Types.h"
#include "CPU/Bus.h"
#include "Model3/DSB.h"

class CSoundBoard: public IBus
{
public:
  // 68000 address space
  UINT8 Read8(UINT32 addr);
  UINT16 Read16(UINT32 addr);
  UINT32 Read32(UINT32 addr);
  void Write8(UINT32 addr, UINT8 data);
  void Write16(UINT32 addr, UINT16 data);
  void Write32(UINT32 addr, UINT32 data);

  // A byte from the PowerPC (to the SCSP MIDI port and the DSB).
  void WriteMIDIPort(UINT8 data);

  // Save states: block "Sound Board", then the 68000, SCSPs and DSB.
  void SaveState(CBlockFile *SaveState);
  void LoadState(CBlockFile *SaveState);

  // Runs one frame (1/60 s) and outputs its audio; returns what the audio
  // output returns (whether its buffer is full).
  bool RunFrame(void);
  void Reset(void);

  // Connects the Digital Sound Board (optional).
  void AttachDSB(CDSB *DSBPtr);

  M68KCtx *GetM68K(void);
  CDSB *GetDSB(void);

  // Once, before anything else: the 68000 program ROM (512 KB) and the
  // sample ROM (16 MB, banked into 0x800000-0xFFFFFF).
  Result Init(const UINT8 *soundROMPtr, const UINT8 *sampleROMPtr);

  // The configuration is kept by reference (it can change while running).
  CSoundBoard(const Util::Config::Node &config);
  ~CSoundBoard(void);

private:
  void SelectSampleBank(void);

  const Util::Config::Node &m_config;
  CDSB *m_dsb = nullptr;
  M68KCtx m_cpu;

  const UINT8 *m_programROM = nullptr;
  const UINT8 *m_sampleROM = nullptr;
  const UINT8 *m_sampleWindow = nullptr;   // the 8 MB of sample ROM seen at 0x800000
  UINT8 *m_memory = nullptr;               // RAM and audio buffers (one block, see SoundBoard.cpp)
  UINT8 *m_ram[2] = { nullptr, nullptr };  // master and slave SCSP RAM, 1 MB each
  float *m_audio[4] = { nullptr, nullptr, nullptr, nullptr };   // front L/R, rear L/R
  UINT8 m_bankReg = 0;                     // 0x400001: bit 4 selects the upper sample bank
};

#endif // INCLUDED_SOUNDBOARD_H
