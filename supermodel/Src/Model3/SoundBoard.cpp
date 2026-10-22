// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The Model 3 sound board (see SoundBoard.h).
//
// 68000 memory map (24-bit addresses, decoded on A23-A20):
//   000000-0FFFFF  master SCSP RAM (1 MB; its first 16 bytes come from the
//                  program ROM at reset: the vector table)
//   100000-1FFFFF  master SCSP registers (64 KB, mirrored)
//   200000-2FFFFF  slave SCSP RAM
//   300000-3FFFFF  slave SCSP registers
//   400001         sample bank register (write): bit 4 = upper 8 MB
//   600000-6FFFFF  program ROM (512 KB, mirrored)
//   800000-FFFFFF  sample ROM, the selected 8 MB bank
// The 68000 is big-endian; RAM and ROM are kept as 16-bit words in host
// order, so byte accesses flip the low address bit.

#include "SoundBoard.h"

#include "Supermodel.h"
#include "OSD/Audio.h"
#include "Sound/SCSP.h"

#include <algorithm>
#include <cstring>

namespace
{
  // The RAM of both chips and the four output buffers are one block in this
  // order: a voice reading past the end of its chip's RAM (the sample
  // address plus play position can exceed 1 MB) reads what follows, as it
  // always has.
  constexpr size_t kRamSize = 0x100000;
  constexpr size_t kChannelBytes = sizeof(float) * DSB_SAMPLES_PER_FRAME;
  constexpr size_t kMemorySize = 2 * kRamSize + 4 * kChannelBytes;

  inline UINT16 Word(const UINT8 *p) { return *reinterpret_cast<const UINT16 *>(p); }
  inline void SetWord(UINT8 *p, UINT16 v) { *reinterpret_cast<UINT16 *>(p) = v; }

  // SCSP callbacks into the 68000.
  int AcknowledgeIrq(int)
  {
    return M68K_IRQ_AUTOVECTOR;   // the SCSP clears its own interrupt
  }

  void SetIrqLevel(int level)
  {
    M68KSetIRQ(level);
  }

  int Run68K(int cycles)
  {
    return M68KRun(cycles) - cycles;
  }
}

void CSoundBoard::SelectSampleBank(void)
{
  m_sampleWindow = m_sampleROM + ((m_bankReg & 0x10) ? 0x800000 : 0);
}

UINT8 CSoundBoard::Read8(UINT32 a)
{
  switch ((a >> 20) & 0xF)
  {
  case 0x0: return m_ram[0][a ^ 1];
  case 0x1: return SCSP_Master_r8(a);
  case 0x2: return m_ram[1][(a & 0xFFFFF) ^ 1];
  case 0x3: return SCSP_Slave_r8(a);
  case 0x6: return m_programROM[(a & 0x7FFFF) ^ 1];
  case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: case 0xE: case 0xF:
    return m_sampleWindow[(a & 0x7FFFFF) ^ 1];
  default: return 0;
  }
}

UINT16 CSoundBoard::Read16(UINT32 a)
{
  switch ((a >> 20) & 0xF)
  {
  case 0x0: return Word(&m_ram[0][a]);
  case 0x1: return SCSP_Master_r16(a);
  case 0x2: return Word(&m_ram[1][a & 0xFFFFF]);
  case 0x3: return SCSP_Slave_r16(a);
  case 0x6: return Word(&m_programROM[a & 0x7FFFF]);
  case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: case 0xE: case 0xF:
    return Word(&m_sampleWindow[a & 0x7FFFFF]);
  default: return 0;
  }
}

UINT32 CSoundBoard::Read32(UINT32 a)
{
  switch ((a >> 20) & 0xF)
  {
  case 0x0:   // (the second word of a long at the very end runs on into the slave RAM)
    return ((UINT32)Word(&m_ram[0][a]) << 16) | Word(&m_ram[0][a + 2]);
  case 0x1: return SCSP_Master_r32(a);
  case 0x2: return ((UINT32)Word(&m_ram[1][a & 0xFFFFF]) << 16) | Word(&m_ram[1][(a + 2) & 0xFFFFF]);
  case 0x3: return SCSP_Slave_r32(a);
  case 0x6: return ((UINT32)Word(&m_programROM[a & 0x7FFFF]) << 16) | Word(&m_programROM[(a + 2) & 0x7FFFF]);
  case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: case 0xE: case 0xF:
    return ((UINT32)Word(&m_sampleWindow[a & 0x7FFFFF]) << 16) | Word(&m_sampleWindow[(a + 2) & 0x7FFFFF]);
  default: return 0;
  }
}

void CSoundBoard::Write8(UINT32 a, UINT8 d)
{
  switch ((a >> 20) & 0xF)
  {
  case 0x0: m_ram[0][a ^ 1] = d; break;
  case 0x1: SCSP_Master_w8(a, d); break;
  case 0x2: m_ram[1][(a & 0xFFFFF) ^ 1] = d; break;
  case 0x3: SCSP_Slave_w8(a, d); break;
  default:
    if (a == 0x400001)
    {
      m_bankReg = d;
      SelectSampleBank();
    }
    break;
  }
}

void CSoundBoard::Write16(UINT32 a, UINT16 d)
{
  switch ((a >> 20) & 0xF)
  {
  case 0x0: SetWord(&m_ram[0][a], d); break;
  case 0x1: SCSP_Master_w16(a, d); break;
  case 0x2: SetWord(&m_ram[1][a & 0xFFFFF], d); break;
  case 0x3: SCSP_Slave_w16(a, d); break;
  default: break;
  }
}

void CSoundBoard::Write32(UINT32 a, UINT32 d)
{
  switch ((a >> 20) & 0xF)
  {
  case 0x0:
    SetWord(&m_ram[0][a], (UINT16)(d >> 16));
    SetWord(&m_ram[0][a + 2], (UINT16)d);
    break;
  case 0x1: SCSP_Master_w32(a, d); break;
  case 0x2:
    SetWord(&m_ram[1][a & 0xFFFFF], (UINT16)(d >> 16));
    SetWord(&m_ram[1][(a + 2) & 0xFFFFF], (UINT16)d);
    break;
  case 0x3: SCSP_Slave_w32(a, d); break;
  default: break;
  }
}

void CSoundBoard::WriteMIDIPort(UINT8 data)
{
  SCSP_MidiIn(data);
  if (m_dsb != nullptr)
    m_dsb->SendCommand(data);   // the DSB listens to the same commands
}

bool CSoundBoard::RunFrame(void)
{
  if (m_config["EmulateSound"].ValueAs<bool>())
  {
    M68KSetContext(&m_cpu);
    SCSP_Update();
    M68KGetContext(&m_cpu);
  }
  else
  {
    for (float *ch : m_audio)
      std::memset(ch, 0, kChannelBytes);
  }

  // The sound volume setting applies to the SCSP output only.
  const float volume = (float)std::max(0, std::min(200, m_config["SoundVolume"].ValueAs<int>())) * (float)(1.0 / 100.0);
  for (int i = 0; i < DSB_SAMPLES_PER_FRAME; i++)
  {
    for (float *ch : m_audio)
      ch[i] *= volume;
  }

  // DSB music goes to the front channels.
  if (m_dsb != nullptr)
    m_dsb->RunFrame(m_audio[0], m_audio[1]);

  return OutputAudio(DSB_SAMPLES_PER_FRAME, m_audio[0], m_audio[1], m_audio[2], m_audio[3], m_config["FlipStereo"].ValueAs<bool>());
}

void CSoundBoard::Reset(void)
{
  // The 68000 must get a valid state even when sound emulation is off.
  std::memcpy(m_ram[0], m_programROM, 16);
  m_bankReg = 0;
  SelectSampleBank();
  M68KSetContext(&m_cpu);
  M68KReset();
  M68KGetContext(&m_cpu);
  if (m_dsb != nullptr)
    m_dsb->Reset();
}

void CSoundBoard::SaveState(CBlockFile *SaveState)
{
  SaveState->NewBlock("Sound Board", __FILE__);
  SaveState->Write(m_ram[0], kRamSize);
  SaveState->Write(m_ram[1], kRamSize);
  SaveState->Write(&m_bankReg, sizeof(m_bankReg));

  M68KSetContext(&m_cpu);
  M68KSaveState(SaveState, "Sound Board 68K");
  SCSP_SaveState(SaveState);
  if (m_dsb != nullptr)
    m_dsb->SaveState(SaveState);
}

void CSoundBoard::LoadState(CBlockFile *SaveState)
{
  if (SaveState->FindBlock("Sound Board") != Result::OKAY)
  {
    ErrorLog("Unable to load sound board state. Save state file is corrupt.");
    return;
  }
  SaveState->Read(m_ram[0], kRamSize);
  SaveState->Read(m_ram[1], kRamSize);
  SaveState->Read(&m_bankReg, sizeof(m_bankReg));
  SelectSampleBank();

  M68KSetContext(&m_cpu);   // keeps the bus and callbacks when the context is copied back
  M68KLoadState(SaveState, "Sound Board 68K");
  M68KGetContext(&m_cpu);
  SCSP_LoadState(SaveState);
  if (m_dsb != nullptr)
    m_dsb->LoadState(SaveState);
}

void CSoundBoard::AttachDSB(CDSB *DSBPtr)
{
  m_dsb = DSBPtr;
}

Result CSoundBoard::Init(const UINT8 *soundROMPtr, const UINT8 *sampleROMPtr)
{
  m_programROM = soundROMPtr;
  m_sampleROM = sampleROMPtr;
  m_bankReg = 0;
  SelectSampleBank();

  m_memory = new (std::nothrow) UINT8[kMemorySize];
  if (m_memory == nullptr)
    return ErrorLog("Insufficient memory for sound board (needs %1.1f MB).", (float)kMemorySize / (float)0x100000);
  std::memset(m_memory, 0, kMemorySize);
  m_ram[0] = m_memory;
  m_ram[1] = m_memory + kRamSize;
  for (int i = 0; i < 4; ++i)
    m_audio[i] = reinterpret_cast<float *>(m_memory + 2 * kRamSize + i * kChannelBytes);

  M68KSetContext(&m_cpu);
  M68KInit();
  M68KAttachBus(this);
  M68KSetIRQCallback(AcknowledgeIrq);
  M68KGetContext(&m_cpu);

  SCSP_SetBuffers(m_audio[0], m_audio[1], m_audio[2], m_audio[3], DSB_SAMPLES_PER_FRAME);
  SCSP_SetCB(Run68K, SetIrqLevel);
  if (SCSP_Init(m_config, 2) != Result::OKAY)
    return Result::FAIL;
  SCSP_SetRAM(0, m_ram[0]);
  SCSP_SetRAM(1, m_ram[1]);
  return Result::OKAY;
}

M68KCtx *CSoundBoard::GetM68K(void)
{
  return &m_cpu;
}

CDSB *CSoundBoard::GetDSB(void)
{
  return m_dsb;
}

CSoundBoard::CSoundBoard(const Util::Config::Node &config)
  : m_config(config)
{
}

CSoundBoard::~CSoundBoard(void)
{
  SCSP_Deinit();
  m_dsb = nullptr;
  delete[] m_memory;
  m_memory = nullptr;
}
