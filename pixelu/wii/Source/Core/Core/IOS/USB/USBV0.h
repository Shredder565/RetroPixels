// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/IOS/USB/Common.h"

// The first USB interface of IOS, used by /dev/usb/oh0 (up to IOS 56; IOS 57-59 replaced it)
// and /dev/usb/oh1. Transfers are ioctlvs whose parameters each sit in a vector of their own.

namespace IOS::HLE
{
struct IOCtlVRequest;

namespace USB
{
enum V0Requests
{
  IOCTLV_USBV0_CTRLMSG = 0,
  IOCTLV_USBV0_BLKMSG = 1,
  IOCTLV_USBV0_INTRMSG = 2,
  IOCTL_USBV0_SUSPENDDEV = 5,
  IOCTL_USBV0_RESUMEDEV = 6,
  IOCTLV_USBV0_ISOMSG = 9,
  IOCTLV_USBV0_LBLKMSG = 10,
  IOCTLV_USBV0_GETDEVLIST = 12,
  IOCTL_USBV0_GETRHDESCA = 15,
  IOCTLV_USBV0_GETRHPORTSTATUS = 20,
  IOCTLV_USBV0_SETRHPORTSTATUS = 25,
  IOCTL_USBV0_DEVREMOVALHOOK = 26,
  IOCTLV_USBV0_DEVINSERTHOOK = 27,
  IOCTLV_USBV0_DEVICECLASSCHANGE = 28,
  IOCTL_USBV0_RESET_DEVICE = 29,
  IOCTLV_USBV0_DEVINSERTHOOKID = 30,
  IOCTL_USBV0_CANCEL_INSERT_HOOK = 31,
  IOCTLV_USBV0_UNKNOWN_32 = 32,
};

// in: bmRequestType, bRequest, wValue, wIndex, wLength (the last three little-endian);
// io: data.
struct V0CtrlMessage final : CtrlMessage
{
  V0CtrlMessage(EmulationKernel& ios, const IOCtlVRequest& ioctlv);
};

// in: endpoint, length (u16, or u32 for the "long" variant); io: data.
struct V0BulkMessage final : BulkMessage
{
  V0BulkMessage(EmulationKernel& ios, const IOCtlVRequest& ioctlv, bool long_length = false);
};

// in: endpoint, length (u16); io: data.
struct V0IntrMessage final : IntrMessage
{
  V0IntrMessage(EmulationKernel& ios, const IOCtlVRequest& ioctlv);
};

// in: endpoint, length (u16), packet count; io: packet lengths, data.
struct V0IsoMessage final : IsoMessage
{
  V0IsoMessage(EmulationKernel& ios, const IOCtlVRequest& ioctlv);
};
}  // namespace USB
}  // namespace IOS::HLE
