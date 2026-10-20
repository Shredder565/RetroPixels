// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/VersionInfo.h"

#include <algorithm>
#include <array>

#include "Common/CommonTypes.h"
#include "Core/CommonTitles.h"
#include "Core/IOS/ES/Formats.h"

namespace IOS::HLE
{
namespace
{
// Unused low memory words are filled with this marker by IOS.
constexpr u32 FILLER = 0xDEADBEEF;

// Where the top of MEM2 (and so the IPC buffer and the IOS heap) lies. Kernels older than
// IOS28 all share the first layout; IOS28 has its own; everything after uses the third.
enum class Mem2Layout
{
  Before28,
  Ios28,
  After28,
};

struct Mem2Top
{
  u32 end;
  u32 ipc_begin;
  u32 reserved_begin;
  u32 reserved_end;
};

constexpr Mem2Top GetMem2Top(Mem2Layout layout)
{
  switch (layout)
  {
  case Mem2Layout::Before28:
    return {0x93400000, 0x933E0000, FILLER, FILLER};
  case Mem2Layout::Ios28:
    return {0x93800000, 0x937E0000, 0x93800000, 0x93820000};
  case Mem2Layout::After28:
  default:
    return {0x93600000, 0x935E0000, 0x93600000, 0x93620000};
  }
}

// Builds one table row. Everything that does not depend on the IOS version is the same for
// all kernels: 24 MiB of MEM1 fully given to the arena, 64 MiB of MEM2 with its arena starting
// at 0x90000800, Hollywood revision 0x11.
constexpr MemoryValues Row(u16 number, u32 version, u32 date, Mem2Layout layout,
                           u32 ram_vendor = 0x0000FF01, bool is_mios = false)
{
  constexpr u32 mem1_bytes = 0x01800000;
  constexpr u32 mem2_bytes = 0x04000000;
  const Mem2Top top = GetMem2Top(layout);

  MemoryValues row{};
  row.ios_number = number;
  row.ios_version = version;
  row.ios_date = date;
  row.mem1_physical_size = mem1_bytes;
  row.mem1_simulated_size = mem1_bytes;
  row.mem1_end = 0x80000000 + mem1_bytes;
  row.mem1_arena_begin = 0;
  row.mem1_arena_end = 0x80000000 + mem1_bytes;
  row.mem2_physical_size = mem2_bytes;
  row.mem2_simulated_size = mem2_bytes;
  row.mem2_end = top.end;
  row.mem2_arena_begin = 0x90000800;
  row.mem2_arena_end = top.ipc_begin;
  row.ipc_buffer_begin = top.ipc_begin;
  row.ipc_buffer_end = top.end;
  row.hollywood_revision = 0x11;
  row.ram_vendor = ram_vendor;
  row.ios_reserved_begin = is_mios ? FILLER : top.reserved_begin;
  row.ios_reserved_end = is_mios ? FILLER : top.reserved_end;
  row.sysmenu_sync = is_mios ? FILLER : 0;
  return row;
}

constexpr auto B = Mem2Layout::Before28;
constexpr auto A = Mem2Layout::After28;

// Version words and build dates as found in each kernel's boot code.
constexpr std::array<MemoryValues, 41> s_known_kernels = {{
    Row(4, 0x40003, 0x81006, B),
    Row(9, 0x9040a, 0x30110, B),
    Row(11, 0xb000a, 0x102506, B),
    Row(12, 0xc020e, 0x30110, B),
    Row(13, 0xd0408, 0x30110, B),
    Row(14, 0xe0408, 0x30110, B),
    Row(15, 0xf0408, 0x30110, B),
    Row(17, 0x110408, 0x30110, B),
    Row(20, 0x14000c, 0x102506, B),
    Row(21, 0x15040f, 0x30110, B),
    Row(22, 0x16050e, 0x30110, B),
    Row(28, 0x1c070f, 0x30110, Mem2Layout::Ios28),
    Row(30, 0x1e0a10, 0x40308, A),
    Row(31, 0x1f0e18, 0x30110, A),
    Row(33, 0x210e18, 0x30110, A),
    Row(34, 0x220e18, 0x30110, A),
    Row(35, 0x230e18, 0x30110, A),
    Row(36, 0x240e18, 0x30110, A),
    Row(37, 0x25161f, 0x30110, A),
    Row(38, 0x26101c, 0x30110, A),
    Row(40, 0x280911, 0x22308, A),
    Row(41, 0x290e17, 0x30110, A),
    Row(43, 0x2b0e17, 0x30110, A),
    Row(45, 0x2d0e17, 0x30110, A),
    Row(46, 0x2e0e17, 0x30110, A),
    Row(48, 0x30101c, 0x30110, A),
    Row(50, 0x321319, 0x101008, A),
    Row(51, 0x331219, 0x71108, A),
    Row(52, 0x34161d, 0x101008, A),
    Row(53, 0x35161f, 0x30110, A),
    Row(55, 0x37161f, 0x30110, A),
    Row(56, 0x38161e, 0x30110, A),
    Row(57, 0x39171f, 0x30110, A),
    Row(58, 0x3a1820, 0x30110, A),
    Row(59, 0x3b2421, 0x101811, A),
    Row(60, 0x3c181e, 0x112408, A),
    Row(61, 0x3d161e, 0x30110, A),
    Row(62, 0x3e191e, 0x22712, A),
    Row(70, 0x461a1f, 0x60309, A),
    Row(80, 0x501b20, 0x30310, A),
    // MIOS (1-257), the GameCube compatibility kernel.
    Row(257, 0x707, 0x82209, A, 0xCAFEBABE, true),
}};

constexpr bool Between(u32 value, u32 low, u32 high)
{
  return value >= low && value <= high;
}
}  // namespace

const std::array<MemoryValues, 41>& GetMemoryValues()
{
  return s_known_kernels;
}

Feature GetFeatures(u32 version)
{
  // Kernel, SD and the basic network stack are in every version.
  Feature set = Feature::Core | Feature::SDIO | Feature::SO | Feature::Ethernet;

  // IOS4 is a stripped-down kernel with no further networking.
  if (version != 4)
    set |= Feature::KD | Feature::SSL | Feature::NCD | Feature::WiFi;

  const bool sdhc = version == 48 || Between(version, 56, 62) || version == 70 || version == 80;
  if (sdhc)
    set |= Feature::SDv2;

  // IOS57-59 carry the newer USB stack; 58 and 59 add EHCI, and 59 the WFS modules.
  const bool new_usb = Between(version, 57, 59);
  if (new_usb)
    set |= Feature::NewUSB;
  if (version == 58 || version == 59)
    set |= Feature::EHCI;
  if (version == 59)
    set |= Feature::WFS;

  // The old-style USB keyboard and HIDv4 modules only exist without the new stack, and the
  // keyboard module only from IOS30 on. Which versions have exactly which of the two is not
  // known, so both are offered.
  if (!new_usb)
  {
    if (version >= 30)
      set |= Feature::USB_KBD;
    set |= Feature::USB_HIDv4;
  }

  return set;
}

bool HasFeature(u32 major_version, Feature feature)
{
  return HasFeature(GetFeatures(major_version), feature);
}

bool IsEmulated(u32 major_version)
{
  // BC (1-256) is never really run; launching it switches to MIOS.
  if (major_version == static_cast<u32>(Titles::BC & 0xffffffff))
    return true;

  return std::ranges::any_of(s_known_kernels, [major_version](const MemoryValues& row) {
    return row.ios_number == major_version;
  });
}

bool IsEmulated(u64 title_id)
{
  // Only IOS kernels can be unknown; every other title is fine.
  const bool is_kernel =
      IsTitleType(title_id, ES::TitleType::System) && title_id != Titles::SYSTEM_MENU;
  return !is_kernel || IsEmulated(static_cast<u32>(title_id));
}
}  // namespace IOS::HLE
