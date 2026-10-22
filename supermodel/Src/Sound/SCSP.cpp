// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The Model 3 sound board's two Yamaha YMF292-F (SCSP) sound chips.
//
// Each chip has 32 voices playing 8- or 16-bit PCM from its own 1 MB of RAM,
// each with an envelope generator, pitch and amplitude LFOs (SCSPLFO.cpp),
// frequency modulation from a 64-entry history of voice outputs, a direct
// stereo send and a send to the chip's effect DSP (SCSPDSP.cpp). The first
// chip ("master") also holds the timers, the interrupt controller and the
// MIDI port the PowerPC sends commands through. The sound 68000 is run in
// step with the samples: 256 of its 11.2896 MHz cycles per 44.1 kHz sample.
// The master chip's output goes to the front speakers, the slave's to the
// rear.
//
// Register map, per chip (68000 byte offsets within the chip's 64 KB):
//   000-3FF  32 voices x 32 bytes
//   400-42F  common control registers
//   600-67F  voice output history (64 words)
//   700-77F  DSP coefficients, 780-7BF DSP addresses (mirrored at 7C0)
//   800-BFF  DSP microcode (writing BF0 starts the DSP)
//   C00-EE3  DSP work, memory, mixer, effect and external registers (read)
//
// All arithmetic - gain and envelope tables, envelope rates, interpolation,
// loop handling, FM offset scaling and mixing - is kept exactly as
// RetroPixels' Model 3 build has always produced it, oddities included, so
// that every game sounds as it did. Where that differs from the chip's
// documentation it says so below.

#include "SCSP.h"
#include "Supermodel.h"
#include "SCSPDSP.h"
#include "OSD/Thread.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "SCSPLFO.cpp"

namespace
{
  constexpr int kFrac = 12;                 // fraction bits of play positions and gains
  constexpr int kEnvFrac = 16;              // fraction bits of envelope levels
  constexpr int kEnvMax = 0x3FF << kEnvFrac;
  constexpr int kEnvKeyOn = 0x17F << kEnvFrac;
  constexpr int kCyclesPerSample = 11289600 / 44100;
  constexpr unsigned kMidiInMask = 0xFF;    // 256-byte input FIFO: a frame of PowerPC commands fits
  constexpr unsigned kMidiOutMask = 31;

  enum EnvPhase : int { kAttack = 0, kDecay1 = 1, kDecay2 = 2, kRelease = 3 };

  struct Envelope
  {
    int level;
    int phase;
    int unusedStep;      // only kept for the save-state layout
    int attack, decay1, decay2, release;
    int decayLevel;
    UINT8 hold;
    UINT8 unusedLink;    // only kept for the save-state layout
  };

  struct Voice
  {
    UINT16 reg[16];      // 0x00-0x17 used
    UINT8 on;
    UINT32 start;        // byte offset of the sample in the chip's RAM
    UINT32 pos;          // play position, 20.12 (in samples)
    UINT32 posNext;      // pos + 1 sample (the second interpolation point)
    UINT32 inc;          // pitch: position increment per output sample
    UINT8 backwards;
    int number;
    INT16 unusedPrev;    // only kept for the save-state layout
    Envelope env;
    ScspLfo::Osc pitchLfo, ampLfo;
  };

  struct Chip
  {
    UINT16 ctl[0x18];    // common registers 0x400-0x42F
    Voice voice[32];
    INT16 history[64];   // the last 64 voice outputs, for FM
    UINT8 historyPos;
    char master;
    UINT8 *ram;
    ScspDsp::State dsp;
  };

  // Voice register fields
  inline bool KeyExecute(const Voice &v)       { return v.reg[0] & 0x1000; }
  inline bool KeyOn(const Voice &v)            { return v.reg[0] & 0x0800; }
  inline unsigned SignControl(const Voice &v)  { return (v.reg[0] >> 9) & 3; }
  inline unsigned SourceControl(const Voice &v){ return (v.reg[0] >> 7) & 3; }
  inline unsigned LoopControl(const Voice &v)  { return (v.reg[0] >> 5) & 3; }
  inline bool Pcm8(const Voice &v)             { return v.reg[0] & 0x0010; }
  inline UINT32 StartAddr(const Voice &v)      { return ((UINT32)(v.reg[0] & 0xF) << 16) | v.reg[1]; }
  inline UINT32 LoopStart(const Voice &v)      { return v.reg[2]; }
  inline UINT32 LoopEnd(const Voice &v)        { return v.reg[3]; }
  inline unsigned Decay2Rate(const Voice &v)   { return (v.reg[4] >> 11) & 0x1F; }
  inline unsigned Decay1Rate(const Voice &v)   { return (v.reg[4] >> 6) & 0x1F; }
  inline UINT8 EnvHold(const Voice &v)         { return (UINT8)(v.reg[4] & 0x20); }
  inline unsigned AttackRate(const Voice &v)   { return v.reg[4] & 0x1F; }
  inline bool LoopLink(const Voice &v)         { return v.reg[5] & 0x4000; }
  inline unsigned KeyScale(const Voice &v)     { return (v.reg[5] >> 10) & 0xF; }
  inline unsigned DecayLevel(const Voice &v)   { return (v.reg[5] >> 5) & 0x1F; }
  inline unsigned ReleaseRate(const Voice &v)  { return v.reg[5] & 0x1F; }
  inline bool NoHistory(const Voice &v)        { return v.reg[6] & 0x0200; }   // STWINH
  inline bool DirectOut(const Voice &v)        { return v.reg[6] & 0x0100; }   // SDIR
  inline unsigned TotalLevel(const Voice &v)   { return v.reg[6] & 0xFF; }
  // MDL as this emulator has always decoded it: bits 13-15 only (MDL & 0xE).
  inline unsigned ModLevel(const Voice &v)     { return (v.reg[7] >> 12) & 0x1E; }
  inline unsigned ModInputX(const Voice &v)    { return (v.reg[7] >> 6) & 0x3F; }
  inline unsigned ModInputY(const Voice &v)    { return v.reg[7] & 0x3F; }
  inline unsigned Octave(const Voice &v)       { return (v.reg[8] >> 11) & 0xF; }
  inline unsigned FNumber(const Voice &v)      { return v.reg[8] & 0x3FF; }
  inline unsigned LfoFreq(const Voice &v)      { return (v.reg[9] >> 10) & 0x1F; }
  inline unsigned PitchLfoWave(const Voice &v) { return (v.reg[9] >> 8) & 3; }
  // PLFOS as this emulator has always decoded it: bits 6-8 of the register
  // (PLFOS & 6, plus the low bit of PLFOWS as 8), giving 0, 2, ..., 14.
  inline unsigned PitchLfoDepth(const Voice &v){ return (v.reg[9] >> 5) & 0xE; }
  inline unsigned AmpLfoWave(const Voice &v)   { return (v.reg[9] >> 3) & 3; }
  inline unsigned AmpLfoDepth(const Voice &v)  { return v.reg[9] & 7; }
  inline unsigned DspSendSel(const Voice &v)   { return (v.reg[10] >> 3) & 0xF; }
  inline unsigned DspSendLevel(const Voice &v) { return v.reg[10] & 7; }
  inline unsigned DirectLevel(const Voice &v)  { return (v.reg[11] >> 13) & 7; }
  inline unsigned DirectPan(const Voice &v)    { return (v.reg[11] >> 8) & 0x1F; }
  inline unsigned EffectLevel(const Voice &v)  { return (v.reg[11] >> 5) & 7; }
  inline unsigned EffectPan(const Voice &v)    { return v.reg[11] & 0x1F; }

  // Common register fields (word indices into Chip::ctl)
  enum
  {
    kCtlMaster = 0x00 / 2,    // MEM4B, DAC18B, VER, MVOL
    kCtlRing = 0x02 / 2,      // RBL, RBP
    kCtlMidiIn = 0x04 / 2,    // MIDI status and input byte
    kCtlMidiOut = 0x06 / 2,
    kCtlMonitor = 0x08 / 2,   // MSLC (write), CA, SGC, EG (read)
    kCtlTimerA = 0x18 / 2,
    kCtlIrqEnable = 0x1E / 2, // SCIEB
    kCtlIrqPending = 0x20 / 2,// SCIPD
    kCtlIrqReset = 0x22 / 2,  // SCIRE
    kCtlIrqLevel0 = 0x24 / 2, // SCILV0-2
    kCtlMainEnable = 0x2A / 2,// MCIEB
    kCtlMainPending = 0x2C / 2,
    kCtlMainReset = 0x2E / 2
  };

  // Interrupt sources (bits of SCIEB/SCIPD/SCILVx)
  enum { kIrqMidiIn = 3, kIrqTimerA = 6, kIrqTimerB = 7 };

  // Attack and decay times from key on to full level / from full level to
  // silence, in ms, for rates 0-63 (SCSP manual); 0 = infinite.
  constexpr double kAttackMs[64] =
  {
    100000, 100000, 8100.0, 6900.0, 6000.0, 4800.0, 4000.0, 3400.0,
    3000.0, 2400.0, 2000.0, 1700.0, 1500.0, 1200.0, 1000.0, 860.0,
    760.0, 600.0, 500.0, 430.0, 380.0, 300.0, 250.0, 220.0,
    190.0, 150.0, 130.0, 110.0, 95.0, 76.0, 63.0, 55.0,
    47.0, 38.0, 31.0, 27.0, 24.0, 19.0, 15.0, 13.0,
    12.0, 9.4, 7.9, 6.8, 6.0, 4.7, 3.8, 3.4,
    3.0, 2.4, 2.0, 1.8, 1.6, 1.3, 1.1, 0.93,
    0.85, 0.65, 0.53, 0.44, 0.40, 0.35, 0.0, 0.0
  };
  constexpr double kDecayMs[64] =
  {
    100000, 100000, 118200.0, 101300.0, 88600.0, 70900.0, 59100.0, 50700.0,
    44300.0, 35500.0, 29600.0, 25300.0, 22200.0, 17700.0, 14800.0, 12700.0,
    11100.0, 8900.0, 7400.0, 6300.0, 5500.0, 4400.0, 3700.0, 3200.0,
    2800.0, 2200.0, 1800.0, 1600.0, 1400.0, 1100.0, 920.0, 790.0,
    690.0, 550.0, 460.0, 390.0, 340.0, 270.0, 230.0, 200.0,
    170.0, 140.0, 110.0, 98.0, 85.0, 68.0, 57.0, 49.0,
    43.0, 34.0, 28.0, 25.0, 22.0, 18.0, 14.0, 12.0,
    11.0, 8.5, 7.1, 6.1, 5.4, 4.3, 3.6, 3.1
  };

  // Lookup tables
  int s_attackStep[64], s_decayStep[64];  // envelope change per sample, 16.16
  INT32 s_envGain[0x400];                 // envelope level -> gain (1/4096), 3/32 dB steps
  // Send gains (1/4096) indexed by level (15-13), pan (12-8), TL (7-0).
  int s_gainL[0x10000], s_gainR[0x10000];

  // Chips and shared state
  Chip s_chip[2];
  Chip *s_cur = &s_chip[0];               // the chip the 68000 last addressed
  bool s_haveSlave = false;

  const Util::Config::Node *s_config = nullptr;
  bool s_threaded = false;
  bool s_legacyDsp = false;               // "LegacySoundDSP": the older DSP register mapping
  CMutex *s_midiLock = nullptr;
  int (*s_run68k)(int cycles) = nullptr;
  void (*s_irq68k)(int level) = nullptr;
  float *s_outFL, *s_outFR, *s_outRL, *s_outRR;
  int s_frameSamples = 0;
  int s_overshoot = 0;                    // 68000 cycles run past the last sample

  UINT32 s_levelTimerA, s_levelTimerBC, s_levelMidi;   // decoded SCILV levels
  UINT16 s_mainEnable, s_mainPending;                  // MCIEB / MCIPD (shared by both chips)

  // The MIDI input FIFO. The output FIFO's bytes are stored in the input
  // FIFO's first 32 entries (its own 16-byte buffer is unused but saved), as
  // this emulator has always done; nothing on Model 3 reads them back.
  UINT8 s_midiOutBuf[16];
  UINT8 s_midiOutW = 0, s_midiOutR = 0;
  UINT8 s_midiIn[kMidiInMask + 1];
  UINT8 s_midiOutFill = 0, s_midiInFill = 0;
  UINT8 s_midiInW = 0, s_midiInR = 0;

  int s_timerScale[3];                    // saved; 1 << prescale
  int s_timerCount[3];                    // 8.8 count up; 0xFFFF = stopped/expired

  class MidiGuard
  {
  public:
    MidiGuard() { if (s_threaded) s_midiLock->Lock(); }
    ~MidiGuard() { if (s_threaded) s_midiLock->Unlock(); }
  };

  // 68000 byte order: even offsets are the high byte of a register word.
  inline void PutByte(UINT16 *words, unsigned offset, UINT8 v)
  {
    UINT16 &w = words[offset >> 1];
    w = (offset & 1) ? (UINT16)((w & 0xFF00) | v) : (UINT16)((w & 0x00FF) | (v << 8));
  }

  inline UINT8 GetByte(const UINT16 *words, unsigned offset)
  {
    const UINT16 w = words[offset >> 1];
    return (offset & 1) ? (UINT8)(w & 0xFF) : (UINT8)(w >> 8);
  }

  /*
   * Interrupts
   */

  UINT32 LevelOf(int source)
  {
    UINT32 level = 0;
    for (int bit = 0; bit < 3; ++bit)
    {
      if (s_cur->ctl[kCtlIrqLevel0 + bit] & 0xFF & (1u << source))
        level |= 1u << bit;
    }
    return level;
  }

  void UpdateIrq()
  {
    UINT32 pending = s_chip[0].ctl[kCtlIrqPending];
    const UINT32 enabled = s_chip[0].ctl[kCtlIrqEnable];

    // MIDI input pending while the FIFO holds data. (The flag goes to the
    // chip last addressed, as it always has here.)
    if (s_midiInW != s_midiInR)
    {
      s_cur->ctl[kCtlIrqPending] |= 1u << kIrqMidiIn;
      pending |= 1u << kIrqMidiIn;
    }

    const UINT32 active = pending & enabled;
    UINT32 level = 0;
    if (active & 0x40)
      level = s_levelTimerA;
    if (active & 0x180)
      level = std::max(level, s_levelTimerBC);
    if (active & 0x08)
    {
      level = std::max(level, s_levelMidi);
      s_cur->ctl[kCtlIrqPending] &= ~(1u << kIrqMidiIn);
    }
    s_irq68k((int)level);
  }

  /*
   * Timers: three 8-bit up-counters clocked at 44.1 kHz / 2^prescale.
   */

  void TickTimers()
  {
    Chip &m = s_chip[0];
    for (int t = 0; t < 3; ++t)
    {
      int &count = s_timerCount[t];
      if (count > 0xFF00)
        continue;
      UINT16 &reg = m.ctl[kCtlTimerA + t];
      count += 1 << (8 - ((reg >> 8) & 7));
      if (count > 0xFF00)
      {
        count = 0xFFFF;
        m.ctl[kCtlIrqPending] |= 0x40 << t;
      }
      reg = (UINT16)((reg & 0xFF00) | (count >> 8));
    }
  }

  /*
   * MIDI
   */

  void MidiOutPush(UINT8 v)
  {
    MidiGuard guard;
    s_midiIn[s_midiOutW++] = v;
    s_midiOutW &= kMidiOutMask;
    ++s_midiOutFill;
  }

  /*
   * Voices
   */

  int RateIndex(int base, unsigned rate)
  {
    return std::clamp(base + (int)(rate << 1), 0, 63);
  }

  UINT32 PitchIncrement(const Voice &v)
  {
    // 1.10 frequency (FNS with the implicit 1) scaled by 2^octave into 20.12.
    const int shift = (int)(Octave(v) ^ 8) - 8 + kFrac - 10;
    const UINT32 f = FNumber(v) + (1u << 10);
    return shift >= 0 ? (f << shift) : (f >> -shift);
  }

  void SetupLfos(Voice &v)
  {
    if (PitchLfoDepth(v) != 0)
      ScspLfo::Setup(v.pitchLfo, LfoFreq(v), PitchLfoWave(v), PitchLfoDepth(v), false);
    if (AmpLfoDepth(v) != 0)
      ScspLfo::Setup(v.ampLfo, LfoFreq(v), AmpLfoWave(v), AmpLfoDepth(v), true);
  }

  void SetupEnvelope(Voice &v)
  {
    int keyScale = 0;
    if (KeyScale(v) != 0xF)
      keyScale = ((int)(Octave(v) ^ 8) - 8) + 2 * (int)KeyScale(v) + (int)((FNumber(v) >> 9) & 1);
    v.env.level = kEnvKeyOn;
    v.env.attack = s_attackStep[RateIndex(keyScale, AttackRate(v))];
    v.env.decay1 = s_decayStep[RateIndex(keyScale, Decay1Rate(v))];
    v.env.decay2 = s_decayStep[RateIndex(keyScale, Decay2Rate(v))];
    v.env.release = s_decayStep[RateIndex(keyScale, ReleaseRate(v))];
    v.env.decayLevel = 0x1F - (int)DecayLevel(v);
    v.env.hold = EnvHold(v);
  }

  void StartVoice(Voice &v)
  {
    v.on = 1;
    v.backwards = 0;
    v.posNext = 1u << kFrac;
    v.pos = 0;
    v.start = Pcm8(v) ? StartAddr(v) : (StartAddr(v) & 0x7FFFE);
    v.inc = PitchIncrement(v);
    SetupEnvelope(v);
    v.env.phase = kAttack;
    v.env.level = kEnvKeyOn;
    v.unusedPrev = 0;
    SetupLfos(v);
  }

  void ReleaseVoice(Voice &v)
  {
    v.env.phase = kRelease;
    v.reg[0] &= ~0x0800;
  }

  void StopVoice(Voice &v)
  {
    v.on = 0;
    v.reg[0] &= ~0x0800;
  }

  // Advances the envelope one sample; returns its level in 1/4096 units
  // of the 10-bit envelope (level << 2).
  int StepEnvelope(Voice &v)
  {
    Envelope &e = v.env;
    switch (e.phase)
    {
    case kAttack:
      e.level += e.attack;
      if (e.level >= kEnvMax)
      {
        if (!LoopLink(v))
        {
          // A decay 1 rate this fast skips straight to decay 2.
          e.phase = (e.decay1 >= (1024 << kEnvFrac)) ? kDecay2 : kDecay1;
        }
        e.level = kEnvMax;
      }
      if (e.hold)
        return 0x3FF << (kFrac - 10);
      break;
    case kDecay1:
      e.level = std::max(e.level - e.decay1, 0);
      if ((e.level >> (kEnvFrac + 5)) <= e.decayLevel)
        e.phase = kDecay2;
      break;
    case kDecay2:
      if (Decay2Rate(v) == 0)
        break;
      e.level = std::max(e.level - e.decay2, 0);
      break;
    case kRelease:
      e.level -= e.release;
      if (e.level <= 0)
      {
        e.level = 0;
        StopVoice(v);
      }
      break;
    default:
      return 1 << kFrac;
    }
    return (e.level >> kEnvFrac) << (kFrac - 10);
  }

  // Applies the loop mode to one play position ('p', with 'sample' its
  // integer part as computed before any change this sample).
  void ApplyLoop(Voice &v, UINT32 &p, UINT32 sample)
  {
    const UINT32 ls = LoopStart(v), le = LoopEnd(v);
    const UINT32 lsPos = ls << kFrac, lePos = le << kFrac;
    switch (LoopControl(v))
    {
    case 0:   // one shot
      if (sample >= ls && sample >= le)
        StopVoice(v);
      break;
    case 1:   // forward loop
      if (sample >= le)
        p = lsPos + (p - lePos);
      break;
    case 2:   // reverse loop: play forward to LSA, then LEA backwards to LSA
      if (sample >= ls && !v.backwards)
      {
        p = lePos - (p - lsPos);
        v.backwards = 1;
      }
      else if ((sample < ls || (p & 0x80000000)) && v.backwards)
        p = lePos - (lsPos - p);
      break;
    case 3:   // alternating
      if (sample >= le)
      {
        p = lePos - (p - lePos);
        v.backwards = 1;
      }
      else if ((sample < ls || (p & 0x80000000)) && v.backwards)
      {
        p = lsPos + (lsPos - p);
        v.backwards = 0;
      }
      break;
    }
  }

  // Produces one sample of a playing voice and writes its history entry.
  int RenderVoice(Chip &chip, Voice &v, INT16 *history)
  {
    if (SourceControl(v) != 0)   // noise and silence sources: not emulated
      return 0;

    int inc = (int)v.inc;
    if (PitchLfoDepth(v) != 0)
      inc = (int)((UINT32)inc * (UINT32)ScspLfo::PitchStep(v.pitchLfo)) >> kFrac;

    const bool pcm8 = Pcm8(v);
    UINT32 a1, a2;   // byte offsets of the two interpolation points
    if (pcm8)
    {
      a1 = v.pos >> kFrac;
      a2 = v.posNext >> kFrac;
    }
    else
    {
      a1 = (v.pos >> (kFrac - 1)) & 0x7FFFE;
      a2 = (v.posNext >> (kFrac - 1)) & 0x7FFFE;
    }

    if (ModLevel(v) != 0 || ModInputX(v) != 0 || ModInputY(v) != 0)
    {
      // FM: offset the read position by the mean of two earlier outputs
      // (always taken from the master chip's history).
      const Chip &m = s_chip[0];
      int mod = (m.history[(m.historyPos + ModInputX(v)) & 63] + m.history[(m.historyPos + ModInputY(v)) & 63]) / 2;
      mod *= 1 << 10;
      mod >>= 0x18 - ModLevel(v);
      if (!pcm8)
        mod *= 2;
      a1 += (UINT32)mod;
      a2 += (UINT32)mod;
      const UINT32 mask = pcm8 ? 0x7FFFF : 0x7FFFE;
      a1 &= mask;
      a2 &= mask;
    }

    const UINT8 *data = chip.ram + v.start;
    const int frac = (int)(v.pos & ((1u << kFrac) - 1));
    int sample;
    if (pcm8)
    {
      const int s1 = (INT8)data[a1 ^ 1] * 256;
      const int s2 = (INT8)data[a2 ^ 1] * 256;
      sample = (s1 * ((1 << kFrac) - frac) + s2 * frac) >> kFrac;
    }
    else
    {
      INT16 s1, s2;
      std::memcpy(&s1, data + a1, 2);
      std::memcpy(&s2, data + a2, 2);
      sample = ((int)s1 * ((1 << kFrac) - frac) + (int)s2 * frac) >> kFrac;
    }

    if (SignControl(v) & 1)
      sample ^= 0x7FFF;
    if (SignControl(v) & 2)
      sample = (INT16)(sample ^ 0x8000);

    if (v.backwards)
      v.pos -= (UINT32)inc;
    else
      v.pos += (UINT32)inc;
    v.posNext = v.pos + (1u << kFrac);

    const UINT32 n1 = v.pos >> kFrac;
    const UINT32 n2 = v.posNext >> kFrac;

    if (n1 >= LoopStart(v) && !v.backwards && LoopLink(v) && v.env.phase == kAttack)
      v.env.phase = kDecay1;

    ApplyLoop(v, v.pos, n1);
    ApplyLoop(v, v.posNext, n2);

    if (!DirectOut(v))
    {
      if (AmpLfoDepth(v) != 0)
        sample = (sample * ScspLfo::AmpStep(v.ampLfo)) >> kFrac;
      // The attack is applied linearly, the other phases through the dB curve.
      if (v.env.phase == kAttack)
        sample = (sample * StepEnvelope(v)) >> kFrac;
      else
        sample = (sample * s_envGain[StepEnvelope(v) >> (kFrac - 10)]) >> kFrac;
    }

    if (!NoHistory(v))
    {
      const unsigned tl = DirectOut(v) ? 0 : TotalLevel(v);
      *history = (INT16)((sample * s_gainL[tl | (7 << 13)]) >> (kFrac + 1));
    }

    return sample;
  }

  /*
   * Register side effects
   */

  // 'index' is the byte index of the register image in host (little-endian)
  // order, as passed since the start: a 68000 byte access to offset n uses
  // n ^ 1, a word access n.
  void VoiceWritten(unsigned voiceNum, unsigned index)
  {
    Voice &v = s_cur->voice[voiceNum];
    switch (index & 0x3F)
    {
    case 0x00: case 0x01:
      if (KeyExecute(v))
      {
        for (Voice &w : s_cur->voice)
        {
          if (KeyOn(w) && w.env.phase == kRelease)
            StartVoice(w);
          if (!KeyOn(w))
            ReleaseVoice(w);
        }
        v.reg[0] &= ~0x1000;
      }
      break;
    case 0x10: case 0x11:
      v.inc = PitchIncrement(v);
      break;
    case 0x0A: case 0x0B:
      // Release and decay level take effect at once (without key scaling).
      v.env.release = s_decayStep[RateIndex(0, ReleaseRate(v))];
      v.env.decayLevel = 0x1F - (int)DecayLevel(v);
      break;
    case 0x12: case 0x13:
      SetupLfos(v);
      break;
    }
  }

  void ControlWritten(unsigned index)
  {
    Chip &c = *s_cur;
    switch (index & 0x3F)
    {
    case 0x00: case 0x02: case 0x03:
      c.dsp.ringWords = (8 * 1024) << ((c.ctl[kCtlRing] >> 7) & 3);
      c.dsp.ringBase = c.ctl[kCtlRing] & 0x7F;
      break;
    case 0x06: case 0x07:
      MidiOutPush((UINT8)(c.ctl[kCtlMidiOut] & 0xFF));
      break;
    case 0x08: case 0x09:
      c.ctl[kCtlMonitor] &= 0xF800;
      break;
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
      if (c.master)
      {
        // Writing a timer loads its count (the low bit of the written value
        // is not used here) and sets its prescaler.
        const int t = ((index & 0x3F) - 0x18) >> 1;
        const UINT16 r = s_chip[0].ctl[kCtlTimerA + t];
        s_timerScale[t] = 1 << ((r >> 8) & 7);
        s_timerCount[t] = (r & 0xFE) << 8;
      }
      break;
    case 0x22: case 0x23:
      if (c.master)
      {
        c.ctl[kCtlIrqPending] &= ~c.ctl[kCtlIrqReset];
        // A stopped (expired) timer keeps requesting its interrupt.
        for (int t = 0; t < 3; ++t)
        {
          if (s_timerCount[t] == 0xFFFF)
            c.ctl[kCtlIrqPending] |= 0x40 << t;
        }
        UpdateIrq();
      }
      break;
    case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29:
      if (c.master)
      {
        s_levelTimerA = LevelOf(kIrqTimerA);
        s_levelTimerBC = LevelOf(kIrqTimerB);
        s_levelMidi = LevelOf(kIrqMidiIn);
      }
      break;
    case 0x2B:
      s_mainEnable = c.ctl[kCtlMainEnable];
      break;
    case 0x2E: case 0x2F:
      s_mainPending &= ~c.ctl[kCtlMainReset];
      break;
    }
  }

  // 'offset' is the 68000 byte offset within 0x400-0x4FF.
  void ControlRead(unsigned offset)
  {
    Chip &c = *s_cur;
    switch (offset & 0x3F)
    {
    case 0x04: case 0x05:
    {
      MidiGuard guard;
      UINT16 v = c.ctl[kCtlMidiIn] & 0xFF00;
      v |= s_midiIn[s_midiInR];
      if (s_midiInR != s_midiInW)
        s_midiInR = (UINT8)((s_midiInR + 1) & kMidiInMask);
      s_midiInFill--;
      c.ctl[kCtlMidiIn] = v;
      UpdateIrq();
      break;
    }
    case 0x08: case 0x09:
    {
      // Monitor the voice selected by MSLC: loop address bits (CA), envelope
      // phase (SGC) and level (EG). MSLC reads back as 0.
      const Voice &v = c.voice[(c.ctl[kCtlMonitor] >> 11) & 0x1F];
      const unsigned phase = (unsigned)v.env.phase & 3;
      const unsigned ca = (v.pos >> (kFrac + 12)) & 0xF;
      const unsigned eg = (0x1F - (v.env.level >> (kEnvFrac + 5))) & 0x1F;
      c.ctl[kCtlMonitor] = (UINT16)((ca << 7) | (phase << 5) | eg);
      break;
    }
    case 0x2A: case 0x2B:
      c.ctl[kCtlMainEnable] = s_mainEnable;
      break;
    case 0x2C: case 0x2D:
      c.ctl[kCtlMainPending] = s_mainPending;
      break;
    }
  }

  // Common register words at byte offsets 0x30-0x4F of the 0x400 block alias
  // voice 0's registers (byte accesses mirror up to 0x4FF), as they always
  // have in this emulator; anything past that is dropped.
  UINT16 *ControlWord(unsigned offset)
  {
    if (offset < 0x30)
      return &s_cur->ctl[offset >> 1];
    if (offset < 0x50)
      return &s_cur->voice[0].reg[(offset - 0x30) >> 1];
    return nullptr;
  }

  /*
   * DSP program area
   */

  UINT16 *DspWord(unsigned flatIndex)
  {
    return flatIndex < ScspDsp::kProgramWords ? &s_cur->dsp.program[flatIndex] : nullptr;
  }

  // Word index into the program area for a register offset (0x700-0xBFF),
  // or -1. 'mirrorAddresses': whether 0x7C0-0x7FF mirror the address table.
  int DspIndex(unsigned offset, bool mirrorAddresses)
  {
    if (offset < 0x780)
      return (int)(ScspDsp::kCoefBase + (offset - 0x700) / 2);
    if (offset < 0x7C0)
      return (int)(ScspDsp::kAddrBase + (offset - 0x780) / 2);
    if (offset < 0x800)
      return mirrorAddresses ? (int)(ScspDsp::kAddrBase + (offset - 0x7C0) / 2) : -1;
    if (offset < 0xC00)
      return (int)(ScspDsp::kCodeBase + (offset - 0x800) / 2);
    return -1;
  }

  void DspWrite8(unsigned offset, UINT8 v)
  {
    int i = DspIndex(offset, !s_legacyDsp);
    if (i >= 0)
      PutByte(s_cur->dsp.program, (unsigned)i * 2 + (offset & 1), v);
  }

  void DspWrite16(unsigned offset, UINT16 v)
  {
    int i;
    if (s_legacyDsp && offset >= 0x780 && offset < 0x800)
      i = (int)(ScspDsp::kAddrBase + (offset - 0x780) / 2);   // runs on into the microcode
    else
      i = DspIndex(offset, true);
    if (i >= 0)
      s_cur->dsp.program[i] = v;
  }

  void DspWrite32(unsigned offset, UINT32 v)
  {
    // A long write stores two words in a row; at the end of one table it
    // runs on into the next.
    int i = DspIndex(offset, true);
    if (i < 0)
      return;
    if (UINT16 *w = DspWord((unsigned)i))
      *w = (UINT16)(v >> 16);
    if (UINT16 *w = DspWord((unsigned)i + 1))
      *w = (UINT16)v;
  }

  UINT16 DspRead16(unsigned offset)
  {
    const ScspDsp::State &d = s_cur->dsp;
    if (offset < 0xC00)
    {
      const int i = DspIndex(offset, true);
      return d.program[i];
    }
    if (offset < 0xE00)
    {
      const INT32 t = d.temp[(offset >> 2) & 0x7F];
      return (offset & 2) ? (UINT16)(t & 0xFFFF) : (UINT16)(t >> 16);
    }
    if (offset < 0xE80)
    {
      const INT32 t = d.mems[(offset >> 2) & 0x1F];
      return (offset & 2) ? (UINT16)(t & 0xFFFF) : (UINT16)(t >> 16);
    }
    if (offset < 0xEC0)
    {
      const INT32 t = d.mixs[(offset >> 2) & 0xF];
      return (offset & 2) ? (UINT16)(t & 0xFFFF) : (UINT16)(t >> 16);
    }
    if (offset < 0xEE0)
      return (UINT16)d.efreg[(offset - 0xEC0) / 2];
    if (offset < 0xEE4)
      return (UINT16)d.exts[(offset - 0xEE0) / 2];
    return 0;
  }

  /*
   * Output
   */

  void RenderSamples(int count)
  {
    const float balance = std::max(-100.f, std::min(100.f, s_config->Get("Balance").ValueAs<float>())) * 0.01f;
    const float gain[2] = { 1.0f + balance, 1.0f - balance };

    float *fl = s_outFL, *fr = s_outFR, *rl = s_outRL, *rr = s_outRR;

    for (int n = 0; n < count; ++n)
    {
      int sum[2][2] = { { 0, 0 }, { 0, 0 } };   // [chip][left/right]

      // Voices of both chips are processed in turn: master 0, slave 0,
      // master 1, ... (the slave's FM reads the master's history).
      for (int vn = 0; vn < 32; ++vn)
      {
        for (int c = 0; c < 2; ++c)
        {
          Chip &chip = s_chip[c];
          Voice &v = chip.voice[vn];
          if (v.on)
          {
            const int s = (int)(gain[c] * (float)RenderVoice(chip, v, &chip.history[chip.historyPos]));
            ScspDsp::Send(chip.dsp, (s * s_gainL[TotalLevel(v) | (DspSendLevel(v) << 13)]) >> (kFrac - 2), (int)DspSendSel(v));
            const unsigned g = TotalLevel(v) | (DirectPan(v) << 8) | (DirectLevel(v) << 13);
            sum[c][0] += (s * s_gainL[g]) >> kFrac;
            sum[c][1] += (s * s_gainR[g]) >> kFrac;
          }
          chip.historyPos = (chip.historyPos + 1) & 63;
        }
      }

      ScspDsp::RunSample(s_chip[0].dsp);
      if (s_haveSlave)
        ScspDsp::RunSample(s_chip[1].dsp);

      // Effect returns: EFREG n is mixed with voice n's EFSDL/EFPAN. (The
      // legacy setting uses a different level position, as it always has.)
      for (int i = 0; i < 16; ++i)
      {
        for (int c = 0; c < (s_haveSlave ? 2 : 1); ++c)
        {
          const Voice &v = s_chip[c].voice[i];
          if (!EffectLevel(v))
            continue;
          const UINT16 g = s_legacyDsp
            ? (UINT16)((EffectPan(v) << 8) | (EffectLevel(v) << 14))
            : (UINT16)((EffectPan(v) << 8) | (EffectLevel(v) << 13));
          const int e = s_chip[c].dsp.efreg[i];
          sum[c][0] += (int)(gain[c] * (float)((e * s_gainL[g]) >> kFrac));
          sum[c][1] += (int)(gain[c] * (float)((e * s_gainR[g]) >> kFrac));
        }
      }

      // 18-bit DAC mode outputs the sums as they are, else they are scaled
      // to 16 bits. (The front channels follow the mode of the chip last
      // addressed, as they always have here.)
      for (int c = 0; c < 2; ++c)
      {
        const bool dac18 = (c == 0 ? s_cur : &s_chip[1])->ctl[kCtlMaster] & 0x100;
        int l = sum[c][0], r = sum[c][1];
        if (c == 0 || s_haveSlave)   // (without a rear chip its empty sums go out as they are)
        {
          if (dac18)
          {
            l = std::clamp(l, -131072, 131071);
            r = std::clamp(r, -131072, 131071);
          }
          else
          {
            l = std::clamp(l >> 2, -32768, 32767);
            r = std::clamp(r >> 2, -32768, 32767);
          }
        }
        if (c == 0)
        {
          *fl++ = (float)l;
          *fr++ = (float)r;
        }
        else
        {
          *rl++ = (float)l;
          *rr++ = (float)r;
        }
      }

      TickTimers();
      UpdateIrq();
      s_overshoot = s_run68k(kCyclesPerSample - s_overshoot);
    }
  }

  void BuildTables()
  {
    for (int i = 0; i < 0x400; ++i)
    {
      const float db = (float)(3 * (i - 0x3FF)) / 32.0f;
      const float unit = (float)(1 << kFrac);
      s_envGain[i] = (INT32)(std::pow(10.0, db / 20.0) * unit);
    }

    // TL bits: 0.4, 0.8, 1.5, 3, 6, 12, 24, 48 dB of attenuation. Pan: 3,
    // 6, 12, 24 dB on the side away from the pan direction (bit 4); 15 is
    // off. Level: off, then -36 to 0 dB in 6 dB steps.
    static constexpr float kTlDb[8] = { 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f, 48.0f };
    static constexpr float kPanDb[4] = { 3.0f, 6.0f, 12.0f, 24.0f };
    static constexpr float kLevelDb[8] = { -1000000.0f, -36.0f, -30.0f, -24.0f, -18.0f, -12.0f, -6.0f, 0.0f };
    for (int i = 0; i < 0x10000; ++i)
    {
      const int tl = i & 0xFF, pan = (i >> 8) & 0x1F, level = (i >> 13) & 7;

      float db = 0;
      for (int b = 0; b < 8; ++b)
      {
        if (tl & (1 << b))
          db -= kTlDb[b];
      }
      const float tlGain = powf(10.0f, db / 20.0f);

      db = 0;
      for (int b = 0; b < 4; ++b)
      {
        if (pan & (1 << b))
          db -= kPanDb[b];
      }
      float panGain;
      if ((pan & 0xF) == 0xF)
        panGain = 0.0f;
      else
        panGain = powf(10.0f, db / 20.0f);

      const float left = (pan < 0x10) ? panGain : 1.0f;
      const float right = (pan < 0x10) ? 1.0f : panGain;
      const float levelGain = level ? powf(10.0f, kLevelDb[level] / 20.0f) : 0.0f;

      s_gainL[i] = (int)(UINT32)((float)(1 << kFrac) * (4.0 * left * tlGain * levelGain));
      s_gainR[i] = (int)(UINT32)((float)(1 << kFrac) * (4.0 * right * tlGain * levelGain));
    }

    s_attackStep[0] = s_decayStep[0] = 0;
    s_attackStep[1] = s_decayStep[1] = 0;
    for (int i = 2; i < 64; ++i)
    {
      const double scale = (double)(1 << kEnvFrac);
      if (kAttackMs[i] != 0.0)
        s_attackStep[i] = (int)(((1023 * 1000.0) / (44100.0 * kAttackMs[i])) * scale);
      else
        s_attackStep[i] = 1024 << kEnvFrac;
      s_decayStep[i] = (int)(((1023 * 1000.0) / (44100.0 * kDecayMs[i])) * scale);
    }
  }

  void ResetChip(Chip &c)
  {
    std::memset(c.ctl, 0, sizeof(c.ctl));
    for (Voice &v : c.voice)
    {
      v = Voice{};
      ScspLfo::Clear(v.pitchLfo);
      ScspLfo::Clear(v.ampLfo);
    }
    std::memset(c.history, 0, sizeof(c.history));
    c.historyPos = 0;
    c.master = 0;
    c.ram = nullptr;
    ScspDsp::Reset(c.dsp);
  }

  /*
   * Bus access (on the chip last selected)
   */

  void Write8(UINT32 addr, UINT8 val)
  {
    addr &= 0xFFFF;
    if (addr < 0x400)
    {
      const unsigned vn = addr / 0x20, off = addr & 0x1F;
      PutByte(s_cur->voice[vn].reg, off, val);
      VoiceWritten(vn, (off ^ 1) & 0x1F);
    }
    else if (addr < 0x600)
    {
      const unsigned off = addr & 0xFF;
      if (UINT16 *w = ControlWord(off & ~1u))
        PutByte(w, off & 1, val);
      ControlWritten((addr ^ 1) & 0xFF);
    }
    else if (addr < 0x700)
      s_cur->history[(addr - 0x600) / 2] = val;
    else
    {
      DspWrite8(addr, val);
      if (addr == 0xBF0)
        ScspDsp::Start(s_cur->dsp);
    }
  }

  void Write16(UINT32 addr, UINT16 val)
  {
    addr &= 0xFFFF;
    if (addr < 0x400)
    {
      const unsigned vn = addr / 0x20, off = addr & 0x1F;
      s_cur->voice[vn].reg[off >> 1] = val;
      VoiceWritten(vn, off);
    }
    else if (addr < 0x600)
    {
      if (addr < 0x430)
      {
        s_cur->ctl[(addr & 0x3F) >> 1] = val;
        ControlWritten(addr & 0x3F);
      }
    }
    else if (addr < 0x700)
      s_cur->history[(addr - 0x600) / 2] = (INT16)val;
    else
    {
      DspWrite16(addr, val);
      if (addr == 0xBF0)
        ScspDsp::Start(s_cur->dsp);
    }
  }

  void Write32(UINT32 addr, UINT32 val)
  {
    addr &= 0xFFFF;
    const UINT16 hi = (UINT16)(val >> 16), lo = (UINT16)val;
    if (addr < 0x400)
    {
      const unsigned vn = addr / 0x20, off = addr & 0x1F;
      Voice &v = s_cur->voice[vn];
      v.reg[off >> 1] = hi;
      if ((off >> 1) + 1 < 16)
        v.reg[(off >> 1) + 1] = lo;
      VoiceWritten(vn, off);
      VoiceWritten(vn, off + 2);
    }
    else if (addr < 0x600)
    {
      const unsigned off = addr & 0xFF;
      if (UINT16 *w = ControlWord(off))
        *w = hi;
      if (UINT16 *w = ControlWord(off + 2))
        *w = lo;
      ControlWritten(off);
      ControlWritten(off + 2);
    }
    else if (addr >= 0x700)
    {
      DspWrite32(addr, val);
      if (addr == 0xBF0)
        ScspDsp::Start(s_cur->dsp);
    }
  }

  UINT8 Read8(UINT32 addr)
  {
    addr &= 0xFFFF;
    if (addr < 0x400)
      return GetByte(s_cur->voice[addr / 0x20].reg, addr & 0x1F);
    if (addr < 0x600)
    {
      const unsigned off = addr & 0xFF;
      ControlRead(off);
      const UINT16 *w = ControlWord(off & ~1u);
      return w ? GetByte(w, off & 1) : 0;
    }
    return 0;
  }

  UINT16 Read16(UINT32 addr)
  {
    addr &= 0xFFFF;
    if (addr < 0x400)
      return s_cur->voice[addr / 0x20].reg[(addr & 0x1F) >> 1];
    if (addr < 0x600)
    {
      if (addr >= 0x430)
        return 0;
      ControlRead(addr & 0x3F);
      return s_cur->ctl[(addr & 0x3F) >> 1];
    }
    if (addr < 0x700)
      return (UINT16)s_cur->history[(addr - 0x600) / 2];
    return DspRead16(addr);
  }

  UINT32 Read32(UINT32)
  {
    return 0xFFFFFFFF;   // long reads are not supported by the chip interface here
  }
} // anonymous namespace


/******************************************************************************
 Interface
******************************************************************************/

Result SCSP_Init(const Util::Config::Node &config, int n)
{
  s_config = &config;
  s_threaded = config["MultiThreaded"].ValueAs<bool>();
  s_legacyDsp = config["LegacySoundDSP"].ValueAs<bool>();

  if (n == 2)
  {
    ResetChip(s_chip[1]);
    s_haveSlave = true;
  }
  ResetChip(s_chip[0]);
  s_chip[0].master = 1;
  s_cur = &s_chip[0];

  s_midiInR = s_midiInW = 0;
  s_midiOutR = s_midiOutW = 0;
  s_midiOutFill = 0;
  s_midiInFill = 0;

  BuildTables();

  for (int i = 0; i < 32; ++i)
    s_chip[0].voice[i].number = i;

  ScspLfo::Init();

  s_chip[0].ctl[kCtlIrqPending] = 0;
  for (int &t : s_timerCount)
    t = 0xFFFF;

  s_midiLock = CThread::CreateMutex();
  if (s_midiLock == nullptr)
    return ErrorLog("Unable to create MIDI mutex!");
  return Result::OKAY;
}

void SCSP_Deinit(void)
{
  delete s_midiLock;
  s_midiLock = nullptr;
}

void SCSP_SetRAM(int n, UINT8 *r)
{
  s_chip[n].ram = r;
  s_chip[n].dsp.ram = reinterpret_cast<UINT16 *>(r);
}

void SCSP_SetBuffers(float *leftBufferPtr, float *rightBufferPtr, float *leftRearBufferPtr, float *rightRearBufferPtr, int bufferLength)
{
  s_outFL = leftBufferPtr;
  s_outFR = rightBufferPtr;
  s_outRL = leftRearBufferPtr;
  s_outRR = rightRearBufferPtr;
  s_frameSamples = bufferLength;
}

void SCSP_SetCB(int (*Run68k)(int cycles), void (*Int68k)(int irq))
{
  s_run68k = Run68k;
  s_irq68k = Int68k;
}

void SCSP_Update()
{
  RenderSamples(s_frameSamples);
}

void SCSP_CpuRunScanline() {}
void SCSP_RTECheck() {}

void SCSP_MidiIn(UINT8 val)
{
  MidiGuard guard;
  s_midiIn[s_midiInW++] = val;
  s_midiInW &= kMidiInMask;
  s_midiInFill++;
}

void SCSP_MidiOutW(UINT8 val)
{
  MidiOutPush(val);
}

UINT8 SCSP_MidiOutR()
{
  if (s_midiOutR == s_midiOutW)
    return 0xFF;
  MidiGuard guard;
  const UINT8 v = s_midiIn[s_midiOutR++];
  s_midiOutR &= kMidiOutMask;
  --s_midiOutFill;
  return v;
}

UINT8 SCSP_MidiOutFill()
{
  MidiGuard guard;
  return s_midiOutFill;
}

UINT8 SCSP_MidiInFill()
{
  MidiGuard guard;
  return s_midiInFill;
}

void SCSP_w8(UINT32 addr, UINT8 val) { Write8(addr, val); }
void SCSP_w16(UINT32 addr, UINT16 val) { Write16(addr, val); }
void SCSP_w32(UINT32 addr, UINT32 val) { Write32(addr, val); }
UINT8 SCSP_r8(UINT32 addr) { return Read8(addr); }
UINT16 SCSP_r16(UINT32 addr) { return Read16(addr); }
UINT32 SCSP_r32(UINT32 addr) { return Read32(addr); }

void SCSP_Master_w8(UINT32 addr, UINT8 val) { s_cur = &s_chip[0]; Write8(addr, val); }
void SCSP_Master_w16(UINT32 addr, UINT16 val) { s_cur = &s_chip[0]; Write16(addr, val); }
void SCSP_Master_w32(UINT32 addr, UINT32 val) { s_cur = &s_chip[0]; Write32(addr, val); }
void SCSP_Slave_w8(UINT32 addr, UINT8 val) { s_cur = &s_chip[1]; Write8(addr, val); }
void SCSP_Slave_w16(UINT32 addr, UINT16 val) { s_cur = &s_chip[1]; Write16(addr, val); }
void SCSP_Slave_w32(UINT32 addr, UINT32 val) { s_cur = &s_chip[1]; Write32(addr, val); }
UINT8 SCSP_Master_r8(UINT32 addr) { s_cur = &s_chip[0]; return Read8(addr); }
UINT16 SCSP_Master_r16(UINT32 addr) { s_cur = &s_chip[0]; return Read16(addr); }
UINT32 SCSP_Master_r32(UINT32 addr) { s_cur = &s_chip[0]; return Read32(addr); }
UINT8 SCSP_Slave_r8(UINT32 addr) { s_cur = &s_chip[1]; return Read8(addr); }
UINT16 SCSP_Slave_r16(UINT32 addr) { s_cur = &s_chip[1]; return Read16(addr); }
UINT32 SCSP_Slave_r32(UINT32 addr) { s_cur = &s_chip[1]; return Read32(addr); }


/******************************************************************************
 Save states

 Block "SCSP x 2". The layout (field sizes and order) is the one RetroPixels'
 Model 3 build has always written, so older save states load.
******************************************************************************/

namespace
{
  template <class F>
  void StateFields(F &&io)
  {
    io(&s_levelTimerA, 4);
    io(&s_levelTimerBC, 4);
    io(&s_levelMidi, 4);
    io(s_midiOutBuf, sizeof(s_midiOutBuf));
    io(&s_midiOutW, 1);
    io(&s_midiOutR, 1);
    io(s_midiIn, sizeof(s_midiIn));
    io(&s_midiOutFill, 1);
    io(&s_midiInFill, 1);
    io(&s_midiInW, 1);
    io(&s_midiInR, 1);
    io(s_timerScale, sizeof(s_timerScale));
    io(s_timerCount, sizeof(s_timerCount));
  }
}

void SCSP_SaveState(CBlockFile *StateFile)
{
  StateFile->NewBlock("SCSP x 2", __FILE__);
  StateFields([StateFile](void *p, uint32_t n) { StateFile->Write(p, n); });

  for (Chip &c : s_chip)
  {
    StateFile->Write(c.ctl, sizeof(c.ctl));
    StateFile->Write(&c.historyPos, 1);
    StateFile->Write(&c.master, 1);
    for (Voice &v : c.voice)
    {
      const UINT64 base = v.start;
      const UINT8 phase = (UINT8)v.env.phase;
      StateFile->Write(v.reg, sizeof(v.reg));
      StateFile->Write(&v.on, 1);
      StateFile->Write(&base, 8);
      StateFile->Write(&v.pos, 4);
      StateFile->Write(&v.posNext, 4);
      StateFile->Write(&v.inc, 4);
      StateFile->Write(&v.backwards, 1);
      StateFile->Write(&v.number, 4);
      StateFile->Write(&v.unusedPrev, 2);
      StateFile->Write(&v.env.level, 4);
      StateFile->Write(&phase, 1);
      StateFile->Write(&v.env.unusedStep, 4);
      StateFile->Write(&v.env.attack, 4);
      StateFile->Write(&v.env.decay1, 4);
      StateFile->Write(&v.env.decay2, 4);
      StateFile->Write(&v.env.release, 4);
      StateFile->Write(&v.env.decayLevel, 4);
      StateFile->Write(&v.env.hold, 1);
      StateFile->Write(&v.env.unusedLink, 1);
      StateFile->Write(&v.pitchLfo.phase, 2);
      StateFile->Write(&v.pitchLfo.rate, 4);
      StateFile->Write(&v.ampLfo.phase, 2);
      StateFile->Write(&v.ampLfo.rate, 4);
    }
    ScspDsp::State &d = c.dsp;
    StateFile->Write(&d.ringBase, 4);
    StateFile->Write(&d.ringWords, 4);
    StateFile->Write(&d.program[ScspDsp::kCoefBase], ScspDsp::kCoefWords * 2);
    StateFile->Write(&d.program[ScspDsp::kAddrBase], ScspDsp::kAddrWords * 2);
    StateFile->Write(&d.program[ScspDsp::kCodeBase], ScspDsp::kCodeWords * 2);
    StateFile->Write(d.temp, sizeof(d.temp));
    StateFile->Write(d.mems, sizeof(d.mems));
    StateFile->Write(&d.ringOffset, 4);
    StateFile->Write(d.mixs, sizeof(d.mixs));
    StateFile->Write(d.exts, sizeof(d.exts));
    StateFile->Write(d.efreg, sizeof(d.efreg));
    StateFile->Write(&d.idle, 1);
    StateFile->Write(&d.steps, 4);
  }
}

void SCSP_LoadState(CBlockFile *StateFile)
{
  if (StateFile->FindBlock("SCSP x 2") != Result::OKAY)
  {
    ErrorLog("Unable to load SCSP state. Save state file is corrupt.");
    return;
  }
  StateFields([StateFile](void *p, uint32_t n) { StateFile->Read(p, n); });

  for (Chip &c : s_chip)
  {
    StateFile->Read(c.ctl, sizeof(c.ctl));
    StateFile->Read(&c.historyPos, 1);
    StateFile->Read(&c.master, 1);
    for (Voice &v : c.voice)
    {
      UINT64 base = 0;
      UINT8 phase = 0;
      StateFile->Read(v.reg, sizeof(v.reg));
      StateFile->Read(&v.on, 1);
      StateFile->Read(&base, 8);
      v.start = (UINT32)(base & 0xFFFFF);   // within the chip's 1 MB
      StateFile->Read(&v.pos, 4);
      StateFile->Read(&v.posNext, 4);
      StateFile->Read(&v.inc, 4);
      StateFile->Read(&v.backwards, 1);
      StateFile->Read(&v.number, 4);
      StateFile->Read(&v.unusedPrev, 2);
      StateFile->Read(&v.env.level, 4);
      StateFile->Read(&phase, 1);
      v.env.phase = phase & 3;
      StateFile->Read(&v.env.unusedStep, 4);
      StateFile->Read(&v.env.attack, 4);
      StateFile->Read(&v.env.decay1, 4);
      StateFile->Read(&v.env.decay2, 4);
      StateFile->Read(&v.env.release, 4);
      StateFile->Read(&v.env.decayLevel, 4);
      StateFile->Read(&v.env.hold, 1);
      StateFile->Read(&v.env.unusedLink, 1);
      StateFile->Read(&v.pitchLfo.phase, 2);
      StateFile->Read(&v.pitchLfo.rate, 4);
      StateFile->Read(&v.ampLfo.phase, 2);
      StateFile->Read(&v.ampLfo.rate, 4);
      SetupLfos(v);   // waveform and depth pointers (and rates) from the registers
    }
    ScspDsp::State &d = c.dsp;
    StateFile->Read(&d.ringBase, 4);
    StateFile->Read(&d.ringWords, 4);
    StateFile->Read(&d.program[ScspDsp::kCoefBase], ScspDsp::kCoefWords * 2);
    StateFile->Read(&d.program[ScspDsp::kAddrBase], ScspDsp::kAddrWords * 2);
    StateFile->Read(&d.program[ScspDsp::kCodeBase], ScspDsp::kCodeWords * 2);
    StateFile->Read(d.temp, sizeof(d.temp));
    StateFile->Read(d.mems, sizeof(d.mems));
    StateFile->Read(&d.ringOffset, 4);
    StateFile->Read(d.mixs, sizeof(d.mixs));
    StateFile->Read(d.exts, sizeof(d.exts));
    StateFile->Read(d.efreg, sizeof(d.efreg));
    StateFile->Read(&d.idle, 1);
    StateFile->Read(&d.steps, 4);
  }
}
