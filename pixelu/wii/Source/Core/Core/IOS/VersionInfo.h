// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/CommonTypes.h"

namespace IOS::HLE
{
// The low memory constants an IOS kernel publishes at 0x3100-0x3160 when it boots, together
// with its version and build date. One entry per IOS version that we know about.
struct MemoryValues
{
  u16 ios_number;
  u32 ios_version;
  u32 ios_date;
  u32 mem1_physical_size;
  u32 mem1_simulated_size;
  u32 mem1_end;
  u32 mem1_arena_begin;
  u32 mem1_arena_end;
  u32 mem2_physical_size;
  u32 mem2_simulated_size;
  u32 mem2_end;
  u32 mem2_arena_begin;
  u32 mem2_arena_end;
  u32 ipc_buffer_begin;
  u32 ipc_buffer_end;
  u32 hollywood_revision;
  u32 ram_vendor;
  u32 ios_reserved_begin;
  u32 ios_reserved_end;
  u32 sysmenu_sync;
};

const std::array<MemoryValues, 41>& GetMemoryValues();

// Modules (resource managers) that a given IOS version ships with.
enum class Feature
{
  // Kernel, ES, FS, STM, DI, OH0, OH1
  Core = 1 << 0,
  // SD slot driver
  SDIO = 1 << 1,
  // Networking: sockets and Ethernet, then KD, SSL, NCD and Wi-Fi
  SO = 1 << 2,
  Ethernet = 1 << 3,
  KD = 1 << 4,
  SSL = 1 << 5,
  NCD = 1 << 6,
  WiFi = 1 << 7,
  // USB keyboard
  USB_KBD = 1 << 8,
  // Version 4 of the USB HID interface
  USB_HIDv4 = 1 << 9,
  // SDHC (SD version 2) support
  SDv2 = 1 << 10,
  // The rewritten USB stack (USB, USB_VEN, USB_HUB, USB_MSC, OHCI0, USB_HIDv5)
  NewUSB = 1 << 11,
  // USB 2.0 host controller
  EHCI = 1 << 12,
  // Wii Fit U / WFS storage (WFSSRV, WFSI, USB_SHARED)
  WFS = 1 << 13,
};

constexpr Feature operator|(Feature lhs, Feature rhs)
{
  return static_cast<Feature>(static_cast<int>(lhs) | static_cast<int>(rhs));
}

constexpr Feature& operator|=(Feature& lhs, Feature rhs)
{
  lhs = lhs | rhs;
  return lhs;
}

constexpr bool HasFeature(Feature features, Feature feature)
{
  return (static_cast<int>(features) & static_cast<int>(feature)) != 0;
}

bool HasFeature(u32 major_version, Feature feature);
Feature GetFeatures(u32 major_version);
bool IsEmulated(u32 major_version);
bool IsEmulated(u64 title_id);
}  // namespace IOS::HLE
