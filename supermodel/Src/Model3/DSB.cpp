// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Sega Digital Sound Board (see DSB.h).
//
// The MPEG decoder (Sound/MPEG) produces 32 kHz stereo; each frame decodes
// 32000/60 + 2 samples (the two extra cover the interpolation's look-ahead)
// after the ones the resampler carried over, and the resampler turns them
// into 735 samples at 44.1 kHz.

#include "DSB.h"

#include "Supermodel.h"
#include "Sound/MPEG/MpegAudio.h"

#include <algorithm>
#include <cstring>

namespace
{
  constexpr int kMpegRate = 32000;
  constexpr int kInputSamples = kMpegRate / 60 + 2;
  // Decode buffers: room for 1/60 s at 48 kHz plus two, per channel.
  constexpr size_t kPcmBytes = 2 * (48000 / 60 + 2);
}


/******************************************************************************
 Resampler
******************************************************************************/

void DsbUpsampler::Reset(void)
{
  m_next = 0;
  m_prev = 1 << 8;
}

int DsbUpsampler::Mix(float *outL, float *outR, INT16 *inL, INT16 *inR, UINT8 volumeL, UINT8 volumeR, int sizeOut, int sizeIn, int outRate, int inRate)
{
  // Input advance per output sample, 1/256 units (below 1 when upsampling).
  const int step = (inRate << 8) / outRate;

  const float music = (float)std::max(0, std::min(200, m_config["MusicVolume"].ValueAs<int>())) * (float)(1.0 / 100.0);
  // The 256 undoes the 1/256 weights.
  const float gainL = music * (float)volumeL * (float)(1.0 / (255.0 * 256.0));
  const float gainR = music * (float)volumeR * (float)(1.0 / (255.0 * 256.0));

  int in = 0;
  for (int out = 0; out < sizeOut; ++out)
  {
    const INT32 l = (int)inL[in] * m_prev + (int)inL[in + 1] * m_next;
    const INT32 r = (int)inR[in] * m_prev + (int)inR[in + 1] * m_next;
    outL[out] += (float)l * gainL;
    outR[out] += (float)r * gainR;

    m_prev -= step;
    m_next += step;
    if (m_prev <= 0)
    {
      m_prev += 1 << 8;
      m_next -= 1 << 8;
      ++in;
    }
  }

  // Keep the input not consumed yet for the next frame.
  int kept = 0;
  for (int i = in; i < sizeIn; ++i, ++kept)
  {
    inL[kept] = inL[i];
    inR[kept] = inR[i];
  }
  return kept;
}


/******************************************************************************
 DSB1: Z80

 Memory: 0000-7FFF program ROM, 8000-FFFF RAM.
 Ports:  E0 mode (0 stop, 1 play, 2 play with loop), E2-E4 start (or loop
         start, when playing) high/middle/low byte (reading them gives the
         play position), E5-E7 end (or loop end), E8 volume (0x7F - level),
         E9 stereo, F0 the next command byte (read; clears the interrupt),
         F1 status (bit 0 ready, bit 1 command pending).
 Every pending command byte raises the Z80 interrupt (RST 38h).
******************************************************************************/

UINT8 CDSB1::Read8(UINT32 addr)
{
  if (addr < 0x8000)
    return m_programROM[addr];
  return m_ram[addr & 0x7FFF];
}

void CDSB1::Write8(UINT32 addr, UINT8 data)
{
  if (addr >= 0x8000)
    m_ram[addr & 0x7FFF] = data;
}

void CDSB1::StartStream(void)
{
  // (Mode 2 also starts without a loop: the Z80 sets it up afterwards.)
  m_playLoopStart = 0;
  m_playLoopLength = 0;
  m_playStart = (UINT32)m_start;
  m_playEnd = (UINT32)m_end;
  MpegDec::SetMemory(m_mpegROM, m_start, m_end - m_start, false);
}

void CDSB1::SetLoop(void)
{
  m_playLoopStart = (UINT32)m_loopStart;
  MpegDec::UpdateMemory(m_mpegROM, (int)m_playLoopStart, (int)m_playLoopLength, true);
}

void CDSB1::IOWrite8(UINT32 addr, UINT8 data)
{
  switch (addr & 0xFF)
  {
  case 0xE0:
    m_mode = data;
    if (data == 0)
      MpegDec::Stop();
    else if (data == 1 || data == 2)
      StartStream();
    break;

  case 0xE2: m_startLatch = (m_startLatch & 0x00FFFF) | ((UINT32)data << 16); break;
  case 0xE3: m_startLatch = (m_startLatch & 0xFF00FF) | ((UINT32)data << 8); break;
  case 0xE4:
    m_startLatch = (m_startLatch & 0xFFFF00) | data;
    if (m_mode == 0)
      m_start = (int)m_startLatch;
    else
    {
      // A loop start while playing. With no loop end yet, the loop runs to
      // the stream's end.
      m_loopStart = (int)m_startLatch;
      m_playLoopLength = (UINT32)((m_loopEnd == 0 ? m_end : m_loopEnd) - m_loopStart);
      SetLoop();
    }
    break;

  case 0xE5: m_endLatch = (m_endLatch & 0x00FFFF) | ((UINT32)data << 16); break;
  case 0xE6: m_endLatch = (m_endLatch & 0xFF00FF) | ((UINT32)data << 8); break;
  case 0xE7:
    m_endLatch = (m_endLatch & 0xFFFF00) | data;
    if (m_mode == 0)
      m_end = (int)m_endLatch;
    else
    {
      m_loopEnd = (int)m_endLatch;
      m_playLoopLength = (UINT32)(m_loopEnd - m_loopStart);
      SetLoop();
    }
    break;

  case 0xE8: m_volume = (UINT8)(0x7F - data); break;
  case 0xE9: m_stereo = data; break;
  default: break;   // (F0: command echo, ignored)
  }
}

UINT8 CDSB1::IORead8(UINT32 addr)
{
  switch (addr & 0xFF)
  {
  case 0xE2: return (UINT8)((MpegDec::GetPosition() + m_start) >> 16);
  case 0xE3: return (UINT8)((MpegDec::GetPosition() + m_start) >> 8);
  case 0xE4: return (UINT8)(MpegDec::GetPosition() + m_start);
  case 0xF0:
  {
    // The next command byte (the last one again when there is none).
    const UINT8 d = m_fifo[m_fifoRead];
    if (m_fifoRead != m_fifoWrite)
      m_fifoRead = (m_fifoRead + 1) & 0xFF;
    if (m_fifoRead == m_fifoWrite)
      m_status &= ~2;
    else
      m_status |= 2;
    m_z80.SetINT(false);
    return d;
  }
  case 0xF1:
    // Bit 0 must be set; Star Wars Arcade polls bit 1 and needs bits 3-5 clear.
    return m_status;
  default:
    return 0;
  }
}

static int DSB1InterruptVector(CZ80 *)
{
  return 0x38;
}

void CDSB1::SendCommand(UINT8 data)
{
  // The Z80 is not run in step with the PowerPC, so a frame's commands are
  // queued here and handed over when it runs.
  m_fifo[m_fifoWrite] = data;
  m_fifoWrite = (m_fifoWrite + 1) & 0xFF;
}

void CDSB1::RunFrame(float *audioL, float *audioR)
{
  if (!m_config["EmulateDSB"].ValueAs<bool>())
  {
    // Silence, but still through the resampler so the output stays aligned.
    std::memset(m_pcm[0], 0, kInputSamples * sizeof(INT16));
    std::memset(m_pcm[1], 0, kInputSamples * sizeof(INT16));
    m_carry = m_resampler.Mix(audioL, audioR, m_pcm[0], m_pcm[1], 0, 0, DSB_SAMPLES_PER_FRAME, kInputSamples, 44100, kMpegRate);
    return;
  }

  // 4 MHz Z80: interrupt for each pending command, then the rest of the frame.
  int cycles = 4000000 / 60;
  while (cycles > 0 && m_fifoRead != m_fifoWrite)
  {
    m_z80.SetINT(true);
    cycles -= m_z80.Run(500);
  }
  m_z80.Run(cycles);

  const UINT8 level = (UINT8)((float)m_volume * (float)(255.0 / 127.0));
  MpegDec::DecodeAudio(&m_pcm[0][m_carry], &m_pcm[1][m_carry], kInputSamples - m_carry);
  m_carry = m_resampler.Mix(audioL, audioR, m_pcm[0], m_pcm[1], level, level, DSB_SAMPLES_PER_FRAME, kInputSamples, 44100, kMpegRate);
}

void CDSB1::Reset(void)
{
  MpegDec::Stop();
  m_resampler.Reset();
  m_carry = 0;
  std::memset(m_fifo, 0, sizeof(m_fifo));
  m_fifoRead = m_fifoWrite = 0;
  m_status = 1;
  m_mode = 0;
  m_volume = 0x7F;
  m_playLoopStart = 0;
  m_z80.Reset();   // also when DSB emulation is off, for a valid Z80 state
}

void CDSB1::SaveState(CBlockFile *StateFile)
{
  const UINT8 loaded = (UINT8)MpegDec::IsLoaded();
  const UINT32 position = (UINT32)MpegDec::GetPosition();
  const UINT32 unused = 0;

  StateFile->NewBlock("DSB1", __FILE__);
  StateFile->Write(&loaded, sizeof(loaded));
  StateFile->Write(&position, sizeof(position));
  StateFile->Write(&unused, sizeof(unused));
  StateFile->Write(&m_playStart, sizeof(m_playStart));
  StateFile->Write(&m_playEnd, sizeof(m_playEnd));
  StateFile->Write(&m_playLoopStart, sizeof(m_playLoopStart));
  StateFile->Write(&m_playLoopLength, sizeof(m_playLoopLength));
  StateFile->Write(m_ram, 0x8000);
  StateFile->Write(m_fifo, sizeof(m_fifo));
  StateFile->Write(&m_fifoRead, sizeof(m_fifoRead));
  StateFile->Write(&m_fifoWrite, sizeof(m_fifoWrite));
  StateFile->Write(&m_start, sizeof(m_start));
  StateFile->Write(&m_end, sizeof(m_end));
  StateFile->Write(&m_mode, sizeof(m_mode));
  StateFile->Write(&m_loopStart, sizeof(m_loopStart));
  StateFile->Write(&m_loopEnd, sizeof(m_loopEnd));
  StateFile->Write(&m_status, sizeof(m_status));
  StateFile->Write(&m_unusedLatch, sizeof(m_unusedLatch));
  StateFile->Write(&m_volume, sizeof(m_volume));
  StateFile->Write(&m_stereo, sizeof(m_stereo));
  m_z80.SaveState(StateFile, "DSB1 Z80");
}

// Restarts the stream a save state was made with, at its position.
static void ResumeStream(const UINT8 *rom, UINT8 loaded, UINT32 position, UINT32 start, UINT32 end, UINT32 loopStart, UINT32 loopLength)
{
  if (loaded)
  {
    MpegDec::SetMemory(rom, (int)start, (int)(end - start), false);
    if (loopLength != 0)
      MpegDec::UpdateMemory(rom, (int)loopStart, (int)loopLength, true);
    MpegDec::SetPosition((int)position);
  }
  else
    MpegDec::Stop();
}

void CDSB1::LoadState(CBlockFile *StateFile)
{
  if (StateFile->FindBlock("DSB1") != Result::OKAY)
  {
    ErrorLog("Unable to load Digital Sound Board state. Save state file is corrupt.");
    return;
  }

  UINT8 loaded = 0;
  UINT32 position = 0, unused = 0;
  StateFile->Read(&loaded, sizeof(loaded));
  StateFile->Read(&position, sizeof(position));
  StateFile->Read(&unused, sizeof(unused));
  StateFile->Read(&m_playStart, sizeof(m_playStart));
  StateFile->Read(&m_playEnd, sizeof(m_playEnd));
  StateFile->Read(&m_playLoopStart, sizeof(m_playLoopStart));
  StateFile->Read(&m_playLoopLength, sizeof(m_playLoopLength));
  StateFile->Read(m_ram, 0x8000);
  StateFile->Read(m_fifo, sizeof(m_fifo));
  StateFile->Read(&m_fifoRead, sizeof(m_fifoRead));
  StateFile->Read(&m_fifoWrite, sizeof(m_fifoWrite));
  StateFile->Read(&m_start, sizeof(m_start));
  StateFile->Read(&m_end, sizeof(m_end));
  StateFile->Read(&m_mode, sizeof(m_mode));
  StateFile->Read(&m_loopStart, sizeof(m_loopStart));
  StateFile->Read(&m_loopEnd, sizeof(m_loopEnd));
  StateFile->Read(&m_status, sizeof(m_status));
  StateFile->Read(&m_unusedLatch, sizeof(m_unusedLatch));
  StateFile->Read(&m_volume, sizeof(m_volume));
  StateFile->Read(&m_stereo, sizeof(m_stereo));
  m_z80.LoadState(StateFile, "DSB1 Z80");

  ResumeStream(m_mpegROM, loaded, position, m_playStart, m_playEnd, m_playLoopStart, m_playLoopLength);
}

Result CDSB1::Init(const UINT8 *progROMPtr, const UINT8 *mpegROMPtr)
{
  m_programROM = progROMPtr;
  m_mpegROM = mpegROMPtr;

  // RAM, then the two decode buffers.
  const size_t size = 0x8000 + 2 * kPcmBytes;
  m_memory = new (std::nothrow) UINT8[size];
  if (m_memory == nullptr)
    return ErrorLog("Insufficient memory for DSB1 board (needs %1.1f MB).", (float)size / (float)0x100000);
  std::memset(m_memory, 0, size);
  m_ram = m_memory;
  m_pcm[0] = reinterpret_cast<INT16 *>(m_memory + 0x8000);
  m_pcm[1] = reinterpret_cast<INT16 *>(m_memory + 0x8000 + kPcmBytes);

  m_z80.Init(this, DSB1InterruptVector);
  m_carry = 0;
  return Result::OKAY;
}

CZ80 *CDSB1::GetZ80(void)
{
  return &m_z80;
}

CDSB1::CDSB1(const Util::Config::Node &config)
  : m_config(config),
    m_resampler(config)
{
}

CDSB1::~CDSB1(void)
{
  delete[] m_memory;
  m_memory = nullptr;
}


/******************************************************************************
 DSB2: 68000

 Memory: 000000-01FFFF program ROM, F00000-F1FFFF RAM (byte reads only up
 to F0FFFF), C00001 the command byte (read), C00003 command valid (reads 1),
 E80001 MPEG status (reads 1), E00003 MPEG command FIFO (write).
 IRQ 1: a command byte is waiting; IRQ 2: 1 kHz timer.

 MPEG commands (as the 68000 programs send them):
   14/15 a b c   start address (a loop start, while playing)
   24/25 a b c   end address (also selects stereo)
   74/75         play            84/85   stop
   A0 n          n != 0: mono from the left stream
   B1 n          n != 0: mono from the right stream
   B0/B6 v       left volume     A1/A7 v right volume
   A4 75         play            B4 96   stop
   A3, A5, B2, B5 n  ignored
******************************************************************************/

namespace
{
  enum Parse
  {
    kIdle = 0,
    kStart0, kStart1, kStart2,   // after 14/15
    kEnd0, kEnd1, kEnd2,         // after 24/25
    kUnused74,                   // (kept so the saved values stay the same)
    kA0, kA1, kA3, kA4, kA5, kA7,
    kB0, kB1, kB2, kB4, kB5, kB6
  };
}

void CDSB2::Play(void)
{
  m_playLoopStart = 0;
  m_playLoopLength = 0;
  m_playStart = (UINT32)m_start;
  m_playEnd = (UINT32)m_end;
  m_playing = 1;
  MpegDec::SetMemory(m_mpegROM, m_start, m_end - m_start, false);
}

void CDSB2::MpegCommand(UINT8 byte)
{
  switch (m_parse)
  {
  case kIdle:
    switch (byte)
    {
    case 0x14: case 0x15: m_parse = kStart0; break;
    case 0x24: case 0x25: m_parse = kEnd0; break;
    case 0x74: case 0x75: Play(); m_parse = kIdle; break;
    case 0x84: case 0x85: MpegDec::Stop(); m_playing = 0; break;
    case 0xA0: m_parse = kA0; break;
    case 0xA1: m_parse = kA1; break;
    case 0xA3: m_parse = kA3; break;
    case 0xA4: m_parse = kA4; break;
    case 0xA5: m_parse = kA5; break;
    case 0xA7: m_parse = kA7; break;
    case 0xB0: m_parse = kB0; break;
    case 0xB1: m_parse = kB1; break;
    case 0xB2: m_parse = kB2; break;
    case 0xB4: m_parse = kB4; break;
    case 0xB5: m_parse = kB5; break;
    case 0xB6: m_parse = kB6; break;
    default: break;
    }
    break;

  case kStart0: m_start = (m_start & 0x00FFFF) | (byte << 16); m_parse = kStart1; break;
  case kStart1: m_start = (m_start & 0xFF00FF) | (byte << 8); m_parse = kStart2; break;
  case kStart2:
    m_start = (m_start & 0xFFFF00) | byte;
    m_parse = kIdle;
    if (m_playing)
    {
      // While playing, a start address sets the loop (to the current end).
      m_playLoopStart = (UINT32)m_start;
      m_playLoopLength = (UINT32)(m_end - m_start);
      MpegDec::UpdateMemory(m_mpegROM, (int)m_playLoopStart, (int)m_playLoopLength, true);
    }
    break;

  case kEnd0: m_end = (m_end & 0x00FFFF) | (byte << 16); m_parse = kEnd1; break;
  case kEnd1: m_end = (m_end & 0xFF00FF) | (byte << 8); m_parse = kEnd2; break;
  case kEnd2:
    m_end = (m_end & 0xFFFF00) | byte;
    m_routing = Routing::Stereo;
    m_parse = kIdle;
    break;

  case kA0: m_routing = byte ? Routing::LeftOnly : Routing::Stereo; m_parse = kIdle; break;
  case kB1: m_routing = byte ? Routing::RightOnly : Routing::Stereo; m_parse = kIdle; break;

  case kA4:   // Daytona 2 plays its attract tune with A4 75
    m_parse = kIdle;
    if (byte == 0x75)
      Play();
    break;
  case kB4:
    m_parse = kIdle;
    if (byte == 0x96)
    {
      MpegDec::Stop();
      m_playing = 0;
    }
    break;

  // Volumes. Daytona 2 sets B6/A7 (rear) and B2/A3 (front); Star Wars
  // Trilogy and Sega Rally 2 use B0/A1.
  case kB0: case kB6: m_volume[0] = byte; m_parse = kIdle; break;
  case kA1: case kA7: m_volume[1] = byte; m_parse = kIdle; break;

  case kA3: case kA5: case kB2: case kB5:
    m_parse = kIdle;
    break;

  default:
    break;
  }
}

UINT8 CDSB2::Read8(UINT32 addr)
{
  if (addr < 0x20000)
    return m_programROM[addr ^ 1];
  if (addr == 0xC00001)
    return (UINT8)m_command;
  if (addr == 0xC00003)
    return 1;
  if (addr == 0xE80001)
    return 1;
  if (addr >= 0xF00000 && addr < 0xF10000)
    return m_ram[(addr & 0x1FFFF) ^ 1];
  return 0;
}

UINT16 CDSB2::Read16(UINT32 addr)
{
  if (addr < 0x20000)
    return *reinterpret_cast<const UINT16 *>(&m_programROM[addr]);
  if (addr >= 0xF00000 && addr < 0xF20000)
    return *reinterpret_cast<const UINT16 *>(&m_ram[addr & 0x1FFFF]);
  return 0;
}

UINT32 CDSB2::Read32(UINT32 addr)
{
  // (A long at the end of RAM or ROM takes its second word from what follows.)
  const UINT8 *p;
  if (addr < 0x20000)
    p = &m_programROM[addr];
  else if (addr >= 0xF00000 && addr < 0xF20000)
    p = &m_ram[addr & 0x1FFFF];
  else
    return 0;
  return ((UINT32)*reinterpret_cast<const UINT16 *>(p) << 16) | *reinterpret_cast<const UINT16 *>(p + 2);
}

void CDSB2::Write8(UINT32 addr, UINT8 data)
{
  if (addr >= 0xF00000 && addr < 0xF20000)
    m_ram[(addr & 0x1FFFF) ^ 1] = data;
  else if (addr == 0xE00003)
    MpegCommand(data);
}

void CDSB2::Write16(UINT32 addr, UINT16 data)
{
  if (addr >= 0xF00000 && addr < 0xF20000)
    *reinterpret_cast<UINT16 *>(&m_ram[addr & 0x1FFFF]) = data;
}

void CDSB2::Write32(UINT32 addr, UINT32 data)
{
  if (addr >= 0xF00000 && addr < 0xF20000)
  {
    UINT8 *p = &m_ram[addr & 0x1FFFF];
    *reinterpret_cast<UINT16 *>(p) = (UINT16)(data >> 16);
    *reinterpret_cast<UINT16 *>(p + 2) = (UINT16)data;
  }
}

void CDSB2::SendCommand(UINT8 data)
{
  m_fifo[m_fifoWrite] = data;
  m_fifoWrite = (m_fifoWrite + 1) & 0xFF;
}

void CDSB2::RunFrame(float *audioL, float *audioR)
{
  if (!m_config["EmulateDSB"].ValueAs<bool>())
  {
    std::memset(m_pcm[0], 0, kInputSamples * sizeof(INT16));
    std::memset(m_pcm[1], 0, kInputSamples * sizeof(INT16));
    m_carry = m_resampler.Mix(audioL, audioR, m_pcm[0], m_pcm[1], 0, 0, DSB_SAMPLES_PER_FRAME, kInputSamples, 44100, kMpegRate);
    return;
  }

  M68KSetContext(&m_cpu);

  // Hand over each queued command byte with IRQ 1.
  while (m_fifoRead != m_fifoWrite)
  {
    m_command = m_fifo[m_fifoRead];
    m_fifoRead = (m_fifoRead + 1) & 0xFF;
    M68KSetIRQ(1);
    m_cycles += M68KRun(500);
  }

  // The rest of the frame (11 MHz), with the timer interrupt every 1 ms.
  while (m_cycles < kCyclesPerFrame)
  {
    if (m_cycles >= m_nextTick)
    {
      M68KSetIRQ(2);
      m_nextTick += kCyclesPerTick;
    }
    m_cycles += M68KRun(std::min(m_nextTick, kCyclesPerFrame) - m_cycles);
  }
  m_cycles -= kCyclesPerFrame;
  m_nextTick -= kCyclesPerFrame;

  M68KGetContext(&m_cpu);

  MpegDec::DecodeAudio(&m_pcm[0][m_carry], &m_pcm[1][m_carry], kInputSamples - m_carry);

  INT16 *srcL = m_pcm[0], *srcR = m_pcm[1];
  UINT8 volL = m_volume[0], volR = m_volume[1];
  if (m_routing == Routing::LeftOnly)
  {
    srcR = m_pcm[0];
    volR = m_volume[0];
  }
  else if (m_routing == Routing::RightOnly)
  {
    srcL = m_pcm[1];
    volL = m_volume[1];
  }
  m_carry = m_resampler.Mix(audioL, audioR, srcL, srcR, volL, volR, DSB_SAMPLES_PER_FRAME, kInputSamples, 44100, kMpegRate);
}

void CDSB2::Reset(void)
{
  MpegDec::Stop();
  m_resampler.Reset();
  m_carry = 0;
  std::memset(m_fifo, 0, sizeof(m_fifo));
  m_fifoRead = m_fifoWrite = 0;
  m_parse = kIdle;
  m_start = 0;
  m_end = 0;
  m_playing = 0;
  m_volume[0] = m_volume[1] = 0xFF;   // full, in case the volume commands are missed
  m_routing = Routing::Stereo;

  M68KSetContext(&m_cpu);   // also when DSB emulation is off, for a valid 68000 state
  M68KReset();
  M68KGetContext(&m_cpu);

  m_cycles = 0;
  m_nextTick = kCyclesPerTick;
}

void CDSB2::SaveState(CBlockFile *StateFile)
{
  const UINT8 loaded = (UINT8)MpegDec::IsLoaded();
  const UINT32 position = (UINT32)MpegDec::GetPosition();
  const UINT32 unused = 0;

  StateFile->NewBlock("DSB2", __FILE__);
  StateFile->Write(&loaded, sizeof(loaded));
  StateFile->Write(&position, sizeof(position));
  StateFile->Write(&unused, sizeof(unused));
  StateFile->Write(&m_playStart, sizeof(m_playStart));
  StateFile->Write(&m_playEnd, sizeof(m_playEnd));
  StateFile->Write(&m_playLoopStart, sizeof(m_playLoopStart));
  StateFile->Write(&m_playLoopLength, sizeof(m_playLoopLength));
  StateFile->Write(m_ram, 0x20000);
  StateFile->Write(m_fifo, sizeof(m_fifo));
  StateFile->Write(&m_fifoRead, sizeof(m_fifoRead));
  StateFile->Write(&m_fifoWrite, sizeof(m_fifoWrite));
  StateFile->Write(&m_command, sizeof(m_command));
  StateFile->Write(&m_parse, sizeof(m_parse));
  StateFile->Write(&m_start, sizeof(m_start));
  StateFile->Write(&m_end, sizeof(m_end));
  StateFile->Write(&m_playing, sizeof(m_playing));
  StateFile->Write(m_volume, sizeof(m_volume));
  StateFile->Write(&m_routing, sizeof(m_routing));

  M68KSetContext(&m_cpu);
  M68KSaveState(StateFile, "DSB2 68K");
}

void CDSB2::LoadState(CBlockFile *StateFile)
{
  if (StateFile->FindBlock("DSB2") != Result::OKAY)
  {
    ErrorLog("Unable to load Digital Sound Board state. Save state file is corrupt.");
    return;
  }

  UINT8 loaded = 0;
  UINT32 position = 0, unused = 0;
  StateFile->Read(&loaded, sizeof(loaded));
  StateFile->Read(&position, sizeof(position));
  StateFile->Read(&unused, sizeof(unused));
  StateFile->Read(&m_playStart, sizeof(m_playStart));
  StateFile->Read(&m_playEnd, sizeof(m_playEnd));
  StateFile->Read(&m_playLoopStart, sizeof(m_playLoopStart));
  StateFile->Read(&m_playLoopLength, sizeof(m_playLoopLength));
  StateFile->Read(m_ram, 0x20000);
  StateFile->Read(m_fifo, sizeof(m_fifo));
  StateFile->Read(&m_fifoRead, sizeof(m_fifoRead));
  StateFile->Read(&m_fifoWrite, sizeof(m_fifoWrite));
  StateFile->Read(&m_command, sizeof(m_command));
  StateFile->Read(&m_parse, sizeof(m_parse));
  StateFile->Read(&m_start, sizeof(m_start));
  StateFile->Read(&m_end, sizeof(m_end));
  StateFile->Read(&m_playing, sizeof(m_playing));
  StateFile->Read(m_volume, sizeof(m_volume));
  StateFile->Read(&m_routing, sizeof(m_routing));

  M68KSetContext(&m_cpu);
  M68KLoadState(StateFile, "DSB2 68K");
  M68KGetContext(&m_cpu);

  // The cycle counters are not in the save state; the frame starts afresh.
  m_cycles = 0;
  m_nextTick = kCyclesPerTick;

  ResumeStream(m_mpegROM, loaded, position, m_playStart, m_playEnd, m_playLoopStart, m_playLoopLength);
}

Result CDSB2::Init(const UINT8 *progROMPtr, const UINT8 *mpegROMPtr)
{
  m_programROM = progROMPtr;
  m_mpegROM = mpegROMPtr;

  const size_t size = 0x20000 + 2 * kPcmBytes;
  m_memory = new (std::nothrow) UINT8[size];
  if (m_memory == nullptr)
    return ErrorLog("Insufficient memory for DSB2 board (needs %1.1f MB).", (float)size / (float)0x100000);
  std::memset(m_memory, 0, size);
  m_ram = m_memory;
  m_pcm[0] = reinterpret_cast<INT16 *>(m_memory + 0x20000);
  m_pcm[1] = reinterpret_cast<INT16 *>(m_memory + 0x20000 + kPcmBytes);

  M68KSetContext(&m_cpu);
  M68KInit();
  M68KAttachBus(this);
  M68KSetIRQCallback(nullptr);   // default: autovector, interrupt cleared on acknowledge
  M68KGetContext(&m_cpu);

  m_carry = 0;
  return Result::OKAY;
}

M68KCtx *CDSB2::GetM68K(void)
{
  return &m_cpu;
}

CDSB2::CDSB2(const Util::Config::Node &config)
  : m_config(config),
    m_resampler(config)
{
}

CDSB2::~CDSB2(void)
{
  delete[] m_memory;
  m_memory = nullptr;
}
