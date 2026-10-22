// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Devices are listed in chain order from TDI to TDO. On TCK rising edges the
// controller captures or shifts and then moves to its next state; on
// falling edges TDO is updated in the shift states and the Update states
// latch the shifted value. ASIC instructions: 3 = IDCODE, 8 = MODEWORD
// (32-bit read/write), anything else BYPASS. Their instruction register is
// 5 bits (captures 00001); the 3D-RAMs' is 4 bits (captures 1001).
//
// Each device's shift register is saved as the 262-character binary string
// (most significant bit first, zero terminated) that older save states hold.

#include "JTAG.h"

#include "Supermodel.h"
#include "Real3D.h"

#include <string>

namespace
{
  const uint8_t kIDCODE = 3;
  const uint8_t kMODEWORD = 8;
  const uint8_t k3DRAMBypass = 15;

  // ID code of a Real3D ASIC: version in bits 31-28, part number 16Cx,
  // manufacturer 057.
  uint32_t ASICID(unsigned version, unsigned part)
  {
    return (uint32_t(version) << 28) | (uint32_t(0x16C0 + part) << 12) | 0x057;
  }

  uint32_t Low32(const std::bitset<262> &b)
  {
    uint32_t v = 0;
    for (unsigned i = 0; i < 32; i++)
      v |= uint32_t(b[i]) << i;
    return v;
  }

  std::bitset<262> From32(uint32_t v)
  {
    std::bitset<262> b;
    for (unsigned i = 0; i < 32; i++)
      b[i] = (v >> i) & 1;
    return b;
  }
}

CJTAG::State CJTAG::Next(State s, bool tms)
{
  switch (s)
  {
  case State::TestLogicReset: return tms ? State::TestLogicReset : State::RunTestIdle;
  case State::RunTestIdle:    return tms ? State::SelectDRScan : State::RunTestIdle;
  case State::SelectDRScan:   return tms ? State::SelectIRScan : State::CaptureDR;
  case State::CaptureDR:      return tms ? State::Exit1DR : State::ShiftDR;
  case State::ShiftDR:        return tms ? State::Exit1DR : State::ShiftDR;
  case State::Exit1DR:        return tms ? State::UpdateDR : State::PauseDR;
  case State::PauseDR:        return tms ? State::Exit2DR : State::PauseDR;
  case State::Exit2DR:        return tms ? State::UpdateDR : State::ShiftDR;
  case State::UpdateDR:       return tms ? State::SelectDRScan : State::RunTestIdle;
  case State::SelectIRScan:   return tms ? State::TestLogicReset : State::CaptureIR;
  case State::CaptureIR:      return tms ? State::Exit1IR : State::ShiftIR;
  case State::ShiftIR:        return tms ? State::Exit1IR : State::ShiftIR;
  case State::Exit1IR:        return tms ? State::UpdateIR : State::PauseIR;
  case State::PauseIR:        return tms ? State::Exit2IR : State::PauseIR;
  case State::Exit2IR:        return tms ? State::UpdateIR : State::ShiftIR;
  case State::UpdateIR:       return tms ? State::SelectDRScan : State::RunTestIdle;
  }
  return State::TestLogicReset;
}

void CJTAG::DeviceReset(Device &d)
{
  if (d.isASIC)
  {
    d.ir = kIDCODE;
    d.length = 32;
    d.shift = From32(d.idCode);
  }
  else
  {
    d.ir = k3DRAMBypass;
    d.length = 1;
    d.shift.reset();
  }
}

void CJTAG::DeviceCaptureIR(Device &d)
{
  if (d.isASIC)
  {
    d.length = 5;
    d.shift = From32(0x01);
  }
  else
  {
    d.length = 4;
    d.shift = From32(0x09);
  }
}

void CJTAG::DeviceCaptureDR(Device &d)
{
  if (d.isASIC && d.ir == kIDCODE)
  {
    d.length = 32;
    d.shift = From32(d.idCode);
  }
  else if (d.isASIC && d.ir == kMODEWORD)
  {
    d.length = 32;
    d.shift = From32(d.modeword);
  }
  else
  {
    d.length = 1;     // BYPASS
    d.shift.reset();
  }
}

void CJTAG::DeviceUpdateIR(Device &d)
{
  d.ir = uint8_t(Low32(d.shift));
}

void CJTAG::DeviceUpdateDR(Device &d)
{
  if (d.isASIC && d.ir == kMODEWORD)
  {
    d.modeword = Low32(d.shift);
    m_real3D.WriteJTAGModeword(d.name, d.modeword);
  }
}

bool CJTAG::DeviceShift(Device &d, bool tdi)
{
  bool out = d.shift[0];
  d.shift >>= 1;
  if (d.length >= 1 && d.length <= kMaxBits)
    d.shift[d.length - 1] = tdi;
  return out;
}

void CJTAG::SaveDevice(CBlockFile *f, const Device &d) const
{
  f->Write(d.shift.to_string());
  f->Write(&d.length, sizeof(d.length));
  f->Write(&d.ir, sizeof(d.ir));
  if (d.isASIC)
  {
    f->Write(&d.idCode, sizeof(d.idCode));
    f->Write(&d.modeword, sizeof(d.modeword));
  }
}

void CJTAG::LoadDevice(CBlockFile *f, Device &d)
{
  char text[kMaxBits + 1] = {};
  f->Read(text, sizeof(text));
  size_t len = 0;
  while (len < kMaxBits && text[len] != '\0')
    len++;
  d.shift.reset();
  for (size_t i = 0; i < len; i++)    // the last character is bit 0
    d.shift[i] = (text[len - 1 - i] == '1');

  f->Read(&d.length, sizeof(d.length));
  f->Read(&d.ir, sizeof(d.ir));
  if (d.isASIC)
  {
    f->Read(&d.idCode, sizeof(d.idCode));
    f->Read(&d.modeword, sizeof(d.modeword));
  }
}

void CJTAG::SaveState(CBlockFile *SaveState)
{
  SaveState->NewBlock("JTAG2", __FILE__);
  int32_t state = int32_t(m_state);
  SaveState->Write(&state, sizeof(state));
  SaveState->Write(&m_lastTck, sizeof(m_lastTck));
  SaveState->Write(&m_tdo, sizeof(m_tdo));
  SaveState->Write(&m_numDevices, sizeof(m_numDevices));
  for (const Device &d : m_chain)
    SaveDevice(SaveState, d);
}

void CJTAG::LoadState(CBlockFile *LoadState)
{
  if (LoadState->FindBlock("JTAG2") != Result::OKAY)
  {
    ErrorLog("Unable to load JTAG state. Save state file is corrupt.");
    return;
  }
  int32_t state = int32_t(m_state);
  LoadState->Read(&state, sizeof(state));
  m_state = State(state & 0xF);
  LoadState->Read(&m_lastTck, sizeof(m_lastTck));
  LoadState->Read(&m_tdo, sizeof(m_tdo));
  LoadState->Read(&m_numDevices, sizeof(m_numDevices));
  if (m_numDevices > kChainLength)
    m_numDevices = kChainLength;
  for (Device &d : m_chain)
    LoadDevice(LoadState, d);
}

bool CJTAG::Read() const
{
  return m_tdo;
}

void CJTAG::Write(bool tck, bool tms, bool tdi, bool trst)
{
  if (!trst)
  {
    // Asynchronous reset of the devices (the controller state is kept).
    for (uint32_t i = 0; i < m_numDevices; i++)
      DeviceReset(m_chain[i]);
    return;
  }

  if (tck == m_lastTck)
    return;
  m_lastTck = tck;

  if (tck)
  {
    switch (m_state)
    {
    case State::TestLogicReset:
      for (uint32_t i = 0; i < m_numDevices; i++)
        DeviceReset(m_chain[i]);
      break;
    case State::CaptureIR:
      for (uint32_t i = 0; i < m_numDevices; i++)
        DeviceCaptureIR(m_chain[i]);
      break;
    case State::CaptureDR:
      for (uint32_t i = 0; i < m_numDevices; i++)
        DeviceCaptureDR(m_chain[i]);
      break;
    case State::ShiftIR:
    case State::ShiftDR:
    {
      bool bit = tdi;
      for (uint32_t i = 0; i < m_numDevices; i++)
        bit = DeviceShift(m_chain[i], bit);
      break;
    }
    default:
      break;
    }
    m_state = Next(m_state, tms);
    return;
  }

  // Falling edge
  switch (m_state)
  {
  case State::ShiftIR:
  case State::ShiftDR:
    if (m_numDevices != 0)
      m_tdo = m_chain[m_numDevices - 1].shift[0];
    break;
  case State::UpdateIR:
    for (uint32_t i = 0; i < m_numDevices; i++)
      DeviceUpdateIR(m_chain[i]);
    break;
  case State::UpdateDR:
    for (uint32_t i = 0; i < m_numDevices; i++)
      DeviceUpdateDR(m_chain[i]);
    break;
  default:
    break;
  }
}

void CJTAG::Reset()
{
  m_state = State::TestLogicReset;
  for (Device &d : m_chain)
    DeviceReset(d);
}

void CJTAG::SetStepping(int stepping)
{
  // ASIC versions per board revision: Mercury, Venus, Earth, Mars, Jupiter.
  struct Versions
  {
    unsigned mercury, venus, earth, mars, jupiter;
  };
  Versions v;
  bool step2;
  switch (stepping)
  {
  case 0x21: v = { 4, 3, 5, 3, 5 }; step2 = true; break;
  case 0x20: v = { 4, 3, 4, 3, 4 }; step2 = true; break;
  case 0x15: v = { 3, 2, 3, 2, 3 }; step2 = false; break;
  default:   v = { 2, 1, 2, 1, 1 }; step2 = false; break;   // 1.0
  }

  // Chain positions (see the constructor).
  m_chain[0].idCode = ASICID(v.jupiter, 7);
  m_chain[1].idCode = ASICID(v.mercury, 3);
  m_chain[2].idCode = ASICID(v.venus, 4);
  m_chain[3].idCode = ASICID(v.earth, 5);
  m_chain[5].idCode = ASICID(v.mars, 6);
  m_chain[6].idCode = ASICID(v.mars, 6);
  if (step2)
  {
    // Second Earth and Mars pair, only on Step 2.x boards
    m_chain[10].idCode = ASICID(v.earth, 5);
    m_chain[12].idCode = ASICID(v.mars, 6);
    m_chain[13].idCode = ASICID(v.mars, 6);
  }
  m_numDevices = step2 ? 17 : 10;
}

CJTAG::CJTAG(CReal3D &real3D)
  : m_real3D(real3D)
{
  // Chain order from TDI: Jupiter, Mercury, Venus, Earth, 3D-RAM, Mars,
  // Mars, three 3D-RAMs, then (Step 2.x) Earth, 3D-RAM, Mars, Mars and
  // three 3D-RAMs. Only the first ASIC of each type configures the
  // renderer.
  const struct
  {
    unsigned pos;
    CASIC::Name name;
  } asics[] =
  {
    { 0, CASIC::Name::Jupiter }, { 1, CASIC::Name::Mercury }, { 2, CASIC::Name::Venus },
    { 3, CASIC::Name::Earth }, { 5, CASIC::Name::Mars }, { 6, CASIC::Name::Dummy },
    { 10, CASIC::Name::Dummy }, { 12, CASIC::Name::Dummy }, { 13, CASIC::Name::Dummy },
  };
  for (const auto &a : asics)
  {
    m_chain[a.pos].isASIC = true;
    m_chain[a.pos].name = a.name;
  }
}
