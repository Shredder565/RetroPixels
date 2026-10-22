// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

// Title IDs of Wii system software that the core refers to by name.
//
// A Wii title ID is 64 bits: the upper word is the title type (00000001 for
// system titles and IOS, 00010001 for downloaded channels, 00010002 for
// built-in channels, 00010008 for hidden ones) and the lower word is either a
// number (IOS) or four ASCII characters (channels). See WiiBrew "Title database".
namespace Titles
{
namespace Detail
{
constexpr u64 Make(u32 type, u32 low)
{
  return (static_cast<u64>(type) << 32) | low;
}

constexpr u64 Channel(u32 type, const char (&code)[5])
{
  const u32 low = (static_cast<u32>(static_cast<u8>(code[0])) << 24) |
                  (static_cast<u32>(static_cast<u8>(code[1])) << 16) |
                  (static_cast<u32>(static_cast<u8>(code[2])) << 8) |
                  static_cast<u32>(static_cast<u8>(code[3]));
  return Make(type, low);
}

constexpr u32 SYSTEM_TYPE = 0x00000001;
constexpr u32 DOWNLOADED_CHANNEL_TYPE = 0x00010001;
constexpr u32 BUILTIN_CHANNEL_TYPE = 0x00010002;
constexpr u32 HIDDEN_CHANNEL_TYPE = 0x00010008;
}  // namespace Detail

// IOS slot N is system title 00000001-0000000N.
constexpr u64 IOS(u32 major_version)
{
  return Detail::Make(Detail::SYSTEM_TYPE, major_version);
}

constexpr u64 BOOT2 = IOS(1);
constexpr u64 SYSTEM_MENU = IOS(2);

// The IOS the last System Menu release (4.3) runs on.
constexpr u64 SYSTEM_MENU_IOS = IOS(80);

// GameCube compatibility layers: BC boots discs, MIOS runs them.
constexpr u64 BC = IOS(0x100);
constexpr u64 MIOS = IOS(0x101);

constexpr u64 SHOP = Detail::Channel(Detail::BUILTIN_CHANNEL_TYPE, "HABA");
constexpr u64 KOREAN_SHOP = Detail::Channel(Detail::BUILTIN_CHANNEL_TYPE, "HABK");

constexpr u64 FORECAST_CHANNEL_NTSC_U = Detail::Channel(Detail::BUILTIN_CHANNEL_TYPE, "HAFE");
constexpr u64 FORECAST_CHANNEL_NTSC_J = Detail::Channel(Detail::BUILTIN_CHANNEL_TYPE, "HAFJ");
constexpr u64 FORECAST_CHANNEL_PAL = Detail::Channel(Detail::BUILTIN_CHANNEL_TYPE, "HAFP");

constexpr u64 NEWS_CHANNEL_NTSC_U = Detail::Channel(Detail::BUILTIN_CHANNEL_TYPE, "HAGE");
constexpr u64 NEWS_CHANNEL_NTSC_J = Detail::Channel(Detail::BUILTIN_CHANNEL_TYPE, "HAGJ");
constexpr u64 NEWS_CHANNEL_PAL = Detail::Channel(Detail::BUILTIN_CHANNEL_TYPE, "HAGP");

constexpr u64 NINTENDO_CHANNEL_NTSC_U = Detail::Channel(Detail::DOWNLOADED_CHANNEL_TYPE, "HATE");
constexpr u64 NINTENDO_CHANNEL_NTSC_J = Detail::Channel(Detail::DOWNLOADED_CHANNEL_TYPE, "HATJ");
constexpr u64 NINTENDO_CHANNEL_PAL = Detail::Channel(Detail::DOWNLOADED_CHANNEL_TYPE, "HATP");

constexpr u64 EVERYBODY_VOTES_CHANNEL_NTSC_U =
    Detail::Channel(Detail::DOWNLOADED_CHANNEL_TYPE, "HAJE");
constexpr u64 EVERYBODY_VOTES_CHANNEL_NTSC_J =
    Detail::Channel(Detail::DOWNLOADED_CHANNEL_TYPE, "HAJJ");
constexpr u64 EVERYBODY_VOTES_CHANNEL_PAL = Detail::Channel(Detail::DOWNLOADED_CHANNEL_TYPE, "HAJP");

constexpr u64 REGION_SELECT_CHANNEL_NTSC_U = Detail::Channel(Detail::HIDDEN_CHANNEL_TYPE, "HALE");
constexpr u64 REGION_SELECT_CHANNEL_NTSC_J = Detail::Channel(Detail::HIDDEN_CHANNEL_TYPE, "HALJ");
constexpr u64 REGION_SELECT_CHANNEL_PAL = Detail::Channel(Detail::HIDDEN_CHANNEL_TYPE, "HALP");

static_assert(SYSTEM_MENU == 0x0000000100000002);
static_assert(SHOP == 0x0001000248414241);
static_assert(KOREAN_SHOP == 0x000100024841424b);
static_assert(FORECAST_CHANNEL_NTSC_U == 0x0001000248414645);
static_assert(FORECAST_CHANNEL_NTSC_J == 0x000100024841464a);
static_assert(FORECAST_CHANNEL_PAL == 0x0001000248414650);
static_assert(NEWS_CHANNEL_NTSC_U == 0x0001000248414745);
static_assert(NEWS_CHANNEL_NTSC_J == 0x000100024841474a);
static_assert(NEWS_CHANNEL_PAL == 0x0001000248414750);
static_assert(NINTENDO_CHANNEL_NTSC_U == 0x0001000148415445);
static_assert(NINTENDO_CHANNEL_NTSC_J == 0x000100014841544a);
static_assert(NINTENDO_CHANNEL_PAL == 0x0001000148415450);
static_assert(EVERYBODY_VOTES_CHANNEL_NTSC_U == 0x0001000148414a45);
static_assert(EVERYBODY_VOTES_CHANNEL_NTSC_J == 0x0001000148414a4a);
static_assert(EVERYBODY_VOTES_CHANNEL_PAL == 0x0001000148414a50);
static_assert(REGION_SELECT_CHANNEL_NTSC_U == 0x0001000848414c45);
static_assert(REGION_SELECT_CHANNEL_NTSC_J == 0x0001000848414c4a);
static_assert(REGION_SELECT_CHANNEL_PAL == 0x0001000848414c50);
static_assert(MIOS == 0x0000000100000101);
}  // namespace Titles
