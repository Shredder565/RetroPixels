// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/IOS/USB/Common.h"

// The USB interface of the early /dev/usb/hid (version 4). Transfers are ioctls whose input
// buffer is one 32-byte request block (see WiiBrew, /dev/usb/hid).

namespace IOS::HLE
{
struct IOCtlRequest;

namespace USB
{
enum V4Requests
{
  IOCTL_USBV4_GETDEVICECHANGE = 0,
  IOCTL_USBV4_SET_SUSPEND = 1,
  IOCTL_USBV4_CTRLMSG = 2,
  IOCTL_USBV4_INTRMSG_IN = 3,
  IOCTL_USBV4_INTRMSG_OUT = 4,
  IOCTL_USBV4_GET_US_STRING = 5,
  IOCTL_USBV4_GETVERSION = 6,
  IOCTL_USBV4_SHUTDOWN = 7,
  IOCTL_USBV4_CANCELINTERRUPT = 8,
};

struct V4CtrlMessage final : CtrlMessage
{
  V4CtrlMessage(EmulationKernel& ios, const IOCtlRequest& ioctl);
};

// Reads a string descriptor in US English and replaces anything that is not printable ASCII
// with '?' before answering, as IOS does.
struct V4GetUSStringMessage final : CtrlMessage
{
  V4GetUSStringMessage(EmulationKernel& ios, const IOCtlRequest& ioctl);
  void OnTransferComplete(s32 return_value) const override;
};

struct V4IntrMessage final : IntrMessage
{
  V4IntrMessage(EmulationKernel& ios, const IOCtlRequest& ioctl);
};
}  // namespace USB
}  // namespace IOS::HLE
