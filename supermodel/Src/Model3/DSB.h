// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Sega's Digital Sound Board: MPEG music for Model 3 games that have it.
// It listens to the same command bytes as the sound board (relayed by
// CSoundBoard) and plays MPEG audio from its own ROM. Two versions exist:
//   CDSB1  Z80 CPU (Scud Race, ...): ports select start/end/loop and volume
//   CDSB2  68000 CPU (Daytona 2, Star Wars Trilogy, ...): commands go to a
//          byte FIFO with a small command language
// Their 32 kHz music is upsampled to 44.1 kHz and mixed into the sound
// board's front channels.

#ifndef INCLUDED_DSB_H
#define INCLUDED_DSB_H

#include "Types.h"
#include "CPU/Bus.h"
#include "CPU/68K/68K.h"
#include "CPU/Z80/Z80.h"
#include "Util/NewConfig.h"

// Samples per channel in one 1/60 s frame at 44.1 kHz.
#define DSB_SAMPLES_PER_FRAME (44100 / 60)

// The interface the sound board uses.
class CDSB: public IBus
{
public:
  // A command byte from the PowerPC.
  virtual void SendCommand(UINT8 data) = 0;
  // Runs one frame and adds its music to these (front) channels.
  virtual void RunFrame(float *audioL, float *audioR) = 0;
  virtual void Reset(void) = 0;
  virtual void SaveState(CBlockFile *SaveState) = 0;
  virtual void LoadState(CBlockFile *SaveState) = 0;
  // Once, before anything else: the CPU program ROM and the MPEG ROM.
  virtual Result Init(const UINT8 *progROMPtr, const UINT8 *mpegROMPtr) = 0;
  virtual ~CDSB() {}
};

// 32 kHz -> 44.1 kHz linear interpolation, carrying the position (and the
// input samples not used yet) from one frame to the next.
class DsbUpsampler
{
public:
  // Mixes sizeOut output samples made from inL/inR (sizeIn samples, plus
  // one readable after them) into outL/outR at the given volumes (0-255) and
  // the "MusicVolume" setting. Moves the unused input to the start of inL/inR
  // and returns how many samples that is.
  int Mix(float *outL, float *outR, INT16 *inL, INT16 *inR, UINT8 volumeL, UINT8 volumeR, int sizeOut, int sizeIn, int outRate, int inRate);
  void Reset(void);
  DsbUpsampler(const Util::Config::Node &config)
    : m_config(config)
  {
    Reset();
  }
private:
  const Util::Config::Node &m_config;
  int m_next;   // weight of the next input sample, 1/256 units
  int m_prev;   // weight of the previous input sample
};

class CDSB1: public CDSB
{
public:
  // Z80 bus
  UINT8 IORead8(UINT32 addr);
  void IOWrite8(UINT32 addr, UINT8 data);
  UINT8 Read8(UINT32 addr);
  void Write8(UINT32 addr, UINT8 data);

  void SendCommand(UINT8 data);
  void RunFrame(float *audioL, float *audioR);
  void Reset(void);
  void SaveState(CBlockFile *StateFile);
  void LoadState(CBlockFile *StateFile);
  Result Init(const UINT8 *progROMPtr, const UINT8 *mpegROMPtr);

  CZ80 *GetZ80(void);

  CDSB1(const Util::Config::Node &config);
  ~CDSB1(void);

private:
  void StartStream(void);
  void SetLoop(void);

  const Util::Config::Node &m_config;
  DsbUpsampler m_resampler;
  int m_carry = 0;                  // input samples carried over by the resampler

  const UINT8 *m_programROM = nullptr;
  const UINT8 *m_mpegROM = nullptr;
  UINT8 *m_memory = nullptr;        // RAM and decode buffers (one block)
  UINT8 *m_ram = nullptr;           // 32 KB
  INT16 *m_pcm[2] = { nullptr, nullptr };

  // Command FIFO (the Z80 reads one byte per interrupt)
  UINT8 m_fifo[256] = {};
  int m_fifoRead = 0;
  int m_fifoWrite = 0;

  // Playback registers
  int m_start = 0, m_end = 0;       // stream (latched while the mode is 0)
  int m_mode = 0;                   // port E0: 0 stop, 1 play, 2 play and loop
  int m_loopStart = 0, m_loopEnd = 0;
  UINT32 m_startLatch = 0, m_endLatch = 0;
  UINT8 m_status = 0;
  UINT8 m_unusedLatch = 0;          // only kept for the save-state layout
  UINT8 m_volume = 0;               // 0x00-0x7F
  UINT8 m_stereo = 0;

  // What the playing stream was started with (for save states)
  UINT32 m_playLoopStart = 0, m_playLoopLength = 0;
  UINT32 m_playStart = 0, m_playEnd = 0;

  CZ80 m_z80;
};

class CDSB2: public CDSB
{
public:
  // 68000 bus
  UINT8 Read8(UINT32 addr);
  UINT16 Read16(UINT32 addr);
  UINT32 Read32(UINT32 addr);
  void Write8(UINT32 addr, UINT8 data);
  void Write16(UINT32 addr, UINT16 data);
  void Write32(UINT32 addr, UINT32 data);

  void SendCommand(UINT8 data);
  void RunFrame(float *audioL, float *audioR);
  void Reset(void);
  void SaveState(CBlockFile *StateFile);
  void LoadState(CBlockFile *StateFile);
  Result Init(const UINT8 *progROMPtr, const UINT8 *mpegROMPtr);

  M68KCtx *GetM68K(void);

  CDSB2(const Util::Config::Node &config);
  ~CDSB2(void);

private:
  // Output channel routing (stored in save states as these values).
  enum class Routing : uint8_t
  {
    Stereo = 0,
    LeftOnly = 1,    // mono from the left stream
    RightOnly = 2    // mono from the right stream
  };

  void MpegCommand(UINT8 byte);
  void Play(void);

  const Util::Config::Node &m_config;
  DsbUpsampler m_resampler;
  int m_carry = 0;

  const UINT8 *m_programROM = nullptr;
  const UINT8 *m_mpegROM = nullptr;
  UINT8 *m_memory = nullptr;        // RAM and decode buffers (one block)
  UINT8 *m_ram = nullptr;           // 128 KB
  INT16 *m_pcm[2] = { nullptr, nullptr };

  UINT8 m_fifo[256] = {};
  int m_fifoRead = 0;
  int m_fifoWrite = 0;

  int m_command = 0;                // the byte the 68000 reads at 0xC00001
  int m_parse = 0;                  // MPEG command parser state
  int m_start = 0, m_end = 0, m_playing = 0;
  UINT8 m_volume[2] = { 0xFF, 0xFF };
  Routing m_routing = Routing::Stereo;

  UINT32 m_playLoopStart = 0, m_playLoopLength = 0;
  UINT32 m_playStart = 0, m_playEnd = 0;

  M68KCtx m_cpu;
  static constexpr int kCyclesPerFrame = 11000000 / 60;
  static constexpr int kCyclesPerTick = 11000000 / 1000;   // the 1 kHz timer (IRQ 2)
  int m_cycles = 0;                 // run so far this frame
  int m_nextTick = kCyclesPerTick;
};

#endif // INCLUDED_DSB_H
