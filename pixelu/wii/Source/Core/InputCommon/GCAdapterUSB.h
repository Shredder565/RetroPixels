// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// Just enough USB to drive the GameCube controller adapter: find a device by vendor and product
// ID, claim its first interface and move data on its interrupt endpoints. Windows uses the
// WinUSB driver (the one players install for the adapter with Zadig); Linux uses the kernel's
// usbfs device files.

#pragma once

#include <memory>
#include <string>

#include "Common/CommonTypes.h"

namespace GCAdapterUSB
{
class Device
{
public:
  // Opens the first present device with this vendor and product ID. Returns null with *found
  // false when there is none, or null with *found true and *error set when one is there but
  // can't be used.
  static std::unique_ptr<Device> Open(u16 vendor_id, u16 product_id, bool* found,
                                      std::string* error);

  virtual ~Device() = default;

  // A control request without a data stage. Returns false on failure.
  virtual bool ControlOut(u8 request_type, u8 request, u16 value, u16 index,
                          unsigned timeout_ms) = 0;

  // Interrupt transfers on interface 0's IN and OUT endpoints. Return the bytes moved, or -1 on
  // failure or timeout (with LastError describing it).
  virtual int ReadInterrupt(u8* data, int size, unsigned timeout_ms) = 0;
  virtual int WriteInterrupt(const u8* data, int size, unsigned timeout_ms) = 0;

  virtual std::string LastError() const = 0;

  // Whether the device was unplugged (later transfers will keep failing).
  virtual bool IsGone() const = 0;
};
}  // namespace GCAdapterUSB
