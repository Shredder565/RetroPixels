// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

namespace IOS
{
// Every IOS resource manager runs as its own process. Its UID and GID are both equal to its
// process ID, which is fixed for each module (see WiiBrew, "IOS/Kernel").
enum IOSUid : u32
{
  PID_KERNEL = 0,
  PID_ES = 1,
  PID_FS = 2,
  PID_DI = 3,
  PID_OH0 = 4,
  PID_OH1 = 5,
  PID_EHCI = 6,
  PID_SDI = 7,
  PID_USBETH = 8,
  PID_NET = 9,
  PID_WD = 10,
  PID_WL = 11,
  PID_KD = 12,
  PID_NCD = 13,
  PID_STM = 14,
  PID_PPCBOOT = 15,
  PID_SSL = 16,
  PID_USB = 17,
  PID_P2P = 18,
  PID_UNKNOWN = 19,
};

// UIDs handed out to PowerPC titles (through /sys/uid.sys) start here.
constexpr u32 FIRST_PPC_UID = 0x1000;

// The System Menu always gets the first PPC UID and group 1.
constexpr u32 SYSMENU_UID = FIRST_PPC_UID;
constexpr u16 SYSMENU_GID = 1;

}  // namespace IOS
