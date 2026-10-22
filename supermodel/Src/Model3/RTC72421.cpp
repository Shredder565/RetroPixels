// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "RTC72421.h"

#include <ctime>
#include "Supermodel.h"

namespace
{
  // The host's local time, looked up again only when the second changes.
  const std::tm &LocalNow()
  {
    static std::time_t s_when = 0;
    static std::tm s_now = {};
    std::time_t t = std::time(nullptr);
    if (t != s_when)
    {
      if (const std::tm *lt = std::localtime(&t))
        s_now = *lt;
      s_when = t;
    }
    return s_now;
  }
}

UINT8 CRTC72421::ReadRegister(unsigned reg)
{
  const std::tm &now = LocalNow();
  int month = now.tm_mon + 1;
  int year = now.tm_year % 100;   // two-digit year

  switch (reg & 0xF)
  {
  case 0x0: return UINT8((now.tm_sec % 10) & 0xF);
  case 0x1: return UINT8((now.tm_sec / 10) & 0x7);
  case 0x2: return UINT8((now.tm_min % 10) & 0xF);
  case 0x3: return UINT8((now.tm_min / 10) & 0x7);
  case 0x4: return UINT8((now.tm_hour % 10) & 0xF);
  case 0x5: return UINT8((now.tm_hour / 10) & 0x7);   // 24-hour mode
  case 0x6: return UINT8((now.tm_mday % 10) & 0xF);
  case 0x7: return UINT8((now.tm_mday / 10) & 0x3);
  case 0x8: return UINT8((month % 10) & 0xF);
  case 0x9: return UINT8((month / 10) & 0x1);
  case 0xA: return UINT8((year % 10) & 0xF);
  case 0xB: return UINT8((year / 10) & 0xF);
  case 0xC: return UINT8(now.tm_wday & 0x7);
  default:  return 0;   // control registers D-F
  }
}

void CRTC72421::WriteRegister(unsigned reg, UINT8 data)
{
  (void)reg;
  (void)data;
}

void CRTC72421::Reset(void)
{
}

void CRTC72421::Init(void)
{
}

CRTC72421::CRTC72421(void)
{
}

CRTC72421::~CRTC72421(void)
{
}
