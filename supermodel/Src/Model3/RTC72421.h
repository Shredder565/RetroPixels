// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Epson RTC-72421 real-time clock. Its sixteen 4-bit registers (seconds,
// minutes, hours, day, month, year as BCD digit pairs, then the day of the
// week and three control registers) read the host's local time. Setting the
// clock is not emulated: writes are ignored.

#ifndef INCLUDED_RTC72421_H
#define INCLUDED_RTC72421_H

#include "Types.h"

class CRTC72421
{
public:
  // reg 0-15; returns the register's 4-bit value.
  UINT8 ReadRegister(unsigned reg);
  void WriteRegister(unsigned reg, UINT8 data);

  void Reset(void);
  void Init(void);

  CRTC72421(void);
  ~CRTC72421(void);
};

#endif // INCLUDED_RTC72421_H
