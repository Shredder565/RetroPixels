// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// 93C46 serial EEPROM: 64 words of 16 bits behind a three-wire interface
// (chip select, clock, data in) plus a data out pin. The Model 3 reaches it
// through the input bank register (F0040000) and reads DO back in bit 5 of
// input bank 1. Commands are a start bit, a 2-bit opcode and a 6-bit
// address (data follows for WRITE and WRAL):
//
//   1 10 aaaaaa          READ     words stream out MSB first from aaaaaa on
//   1 01 aaaaaa d16      WRITE
//   1 11 aaaaaa          ERASE    word := FFFF
//   1 00 11xxxx          EWEN     (recognised after its first 5 bits)
//   1 00 00xxxx          EWDS     (likewise)
//   1 00 01xxxx d16      WRAL
//   1 00 10xxxx          ERAL
//
// Writes and erases only take effect after EWEN. While idle, DO reads 0
// ("busy") for a few reads after chip select drops, then 1 ("ready").

#ifndef INCLUDED_93C46_H
#define INCLUDED_93C46_H

#include "BlockFile.h"
#include "Types.h"

class C93C46
{
public:
  // Save state / NVRAM block "93C46".
  void SaveState(CBlockFile *SaveState);
  void LoadState(CBlockFile *SaveState);

  // Sets every word to FFFF (a blank chip).
  void Clear(void);

  // New levels on the CS, CLK and DI pins (nonzero = high).
  void Write(unsigned pinCS, unsigned pinCLK, unsigned pinDI);

  // The DO pin.
  unsigned Read(void);

  void Reset(void);
  void Init(void);

  C93C46(void);
  ~C93C46(void);

private:
  void OnClockRise(void);
  void Decode(void);
  void LoadOutputWord(void);

  UINT16 m_mem[64];
  unsigned m_cs = 0, m_clk = 0, m_di = 0, m_do = 0;
  UINT32 m_out = 0;       // word being shifted out, next bit in bit 0
  UINT32 m_in = 0;        // bits shifted in since CS went high
  int m_outCount = 0;     // bits of m_out already sent
  bool m_listening = true;  // false while a READ streams data out
  unsigned m_addr = 0;
  int m_busy = 0;         // reads left that report busy
  bool m_protected = true;  // until EWEN
};

#endif // INCLUDED_93C46_H
