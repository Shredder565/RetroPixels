// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The input shift register is matched against each command's bit pattern
// after every clock; leading zeros before the start bit are harmless because
// it is cleared whenever chip select is low. A READ does not send the dummy
// zero bit the datasheet describes before the first word (games expect the
// data bits straight away).

#include "93C46.h"

#include <cstring>
#include "Supermodel.h"

namespace
{
  // The word with its bit order reversed, so that shifting right sends D15
  // first.
  UINT32 MirrorWord(UINT16 w)
  {
    UINT32 r = 0;
    for (int i = 0; i < 16; i++)
    {
      r = (r << 1) | (w & 1);
      w >>= 1;
    }
    return r;
  }
}

void C93C46::SaveState(CBlockFile *SaveState)
{
  SaveState->NewBlock("93C46", __FILE__);
  SaveState->Write(m_mem, sizeof(m_mem));
  SaveState->Write(&m_cs, sizeof(m_cs));
  SaveState->Write(&m_clk, sizeof(m_clk));
  SaveState->Write(&m_di, sizeof(m_di));
  SaveState->Write(&m_do, sizeof(m_do));
  SaveState->Write(&m_out, sizeof(m_out));
  SaveState->Write(&m_in, sizeof(m_in));
  SaveState->Write(&m_outCount, sizeof(m_outCount));
  SaveState->Write(&m_listening, sizeof(m_listening));
  SaveState->Write(&m_addr, sizeof(m_addr));
  SaveState->Write(&m_busy, sizeof(m_busy));
  SaveState->Write(&m_protected, sizeof(m_protected));
}

void C93C46::LoadState(CBlockFile *SaveState)
{
  if (SaveState->FindBlock("93C46") != Result::OKAY)
  {
    ErrorLog("Unable to load EEPROM state. File is corrupt.");
    return;
  }
  SaveState->Read(m_mem, sizeof(m_mem));
  SaveState->Read(&m_cs, sizeof(m_cs));
  SaveState->Read(&m_clk, sizeof(m_clk));
  SaveState->Read(&m_di, sizeof(m_di));
  SaveState->Read(&m_do, sizeof(m_do));
  SaveState->Read(&m_out, sizeof(m_out));
  SaveState->Read(&m_in, sizeof(m_in));
  SaveState->Read(&m_outCount, sizeof(m_outCount));
  SaveState->Read(&m_listening, sizeof(m_listening));
  SaveState->Read(&m_addr, sizeof(m_addr));
  SaveState->Read(&m_busy, sizeof(m_busy));
  SaveState->Read(&m_protected, sizeof(m_protected));
}

void C93C46::LoadOutputWord(void)
{
  m_out = MirrorWord(m_mem[m_addr & 0x3F]);
  m_outCount = 0;
}

void C93C46::Decode(void)
{
  const UINT32 bits = m_in;
  const unsigned addr6 = bits & 0x3F;

  if ((bits >> 6) == 0x6)             // 1 10 aaaaaa: READ
  {
    m_addr = addr6;
    LoadOutputWord();
    m_listening = false;
  }
  else if ((bits >> 6) == 0x7)        // 1 11 aaaaaa: ERASE
  {
    if (!m_protected)
      m_mem[addr6] = 0xFFFF;
    m_do = 1;
  }
  else if (bits == 0x13)              // 1 00 11: EWEN
    m_protected = false;
  else if (bits == 0x10)              // 1 00 00: EWDS
    m_protected = true;
  else if ((bits >> 4) == 0x12)       // 1 00 10 xxxx: ERAL
  {
    if (!m_protected)
    {
      for (UINT16 &w : m_mem)
        w = 0xFFFF;
    }
    m_do = 1;
  }
  else if ((bits >> 22) == 0x5)       // 1 01 aaaaaa d16: WRITE
  {
    if (!m_protected)
      m_mem[(bits >> 16) & 0x3F] = UINT16(bits);
    m_do = 1;
  }
  else if ((bits >> 20) == 0x11)      // 1 00 01 xxxx d16: WRAL
  {
    if (!m_protected)
    {
      for (UINT16 &w : m_mem)
        w = UINT16(bits);
    }
    m_do = 1;
  }
}

void C93C46::OnClockRise(void)
{
  if (m_listening)
  {
    m_in = (m_in << 1) | m_di;
    Decode();
    return;
  }

  // Streaming a READ: one bit per clock, continuing with the next word.
  m_do = m_out & 1;
  m_out >>= 1;
  if (++m_outCount == 16)
  {
    m_addr = (m_addr + 1) & 0x3F;
    LoadOutputWord();
  }
}

void C93C46::Write(unsigned pinCS, unsigned pinCLK, unsigned pinDI)
{
  unsigned lastClk = m_clk;
  m_cs = pinCS ? 1 : 0;
  m_clk = pinCLK ? 1 : 0;
  m_di = pinDI ? 1 : 0;

  if (!m_cs)
  {
    // Deselected: forget any partial command and report busy for a while
    // (some games wait for a write to "complete").
    m_in = 0;
    m_listening = true;
    m_busy = 5;
    return;
  }

  if (!lastClk && m_clk)
    OnClockRise();
}

unsigned C93C46::Read(void)
{
  if (!m_listening)
    return m_do;
  if (m_busy > 0)
  {
    m_busy--;
    return 0;
  }
  return 1;
}

void C93C46::Clear(void)
{
  for (UINT16 &w : m_mem)
    w = 0xFFFF;
}

void C93C46::Reset(void)
{
  m_listening = true;
  m_protected = true;
  m_in = 0;
  m_out = 0;
  m_addr = 0;
  m_busy = 0;
  m_cs = 0;
}

void C93C46::Init(void)
{
}

C93C46::C93C46(void)
{
  Clear();
}

C93C46::~C93C46(void)
{
}
