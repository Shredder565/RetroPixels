// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// The portal protocol (HID output reports whose first byte is a command letter, answered on
// the interrupt endpoint) is described by public community documentation, e.g.
// https://marijnkneppers.dev/posts and https://github.com/tresni/PoweredPortals/wiki/USB-Protocols

#include "Core/IOS/USB/Emulated/Skylanders/Skylander.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "AudioCommon/AudioCommon.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace IOS::HLE::USB
{
namespace
{
struct FigureListEntry
{
  u16 figure_id;
  u16 variant_id;
  Game game;
  Element element;
  Type type;
  const char* name;
};

using G = Game;
using E = Element;
using T = Type;

// clang-format off
constexpr FigureListEntry FIGURE_LIST[] = {
    {0, 0x0000, G::SpyrosAdv, E::Air, T::Skylander, "Whirlwind"},
    {0, 0x1801, G::Giants, E::Air, T::Skylander, "Whirlwind (S2)"},
    {0, 0x1812, G::Giants, E::Air, T::Skylander, "Whirlwind (Stone)"},
    {0, 0x1C02, G::Giants, E::Air, T::Skylander, "Whirlwind (Polar)"},
    {0, 0x2805, G::SwapForce, E::Air, T::Skylander, "Whirlwind (Horn Blast)"},
    {0, 0x3810, G::TrapTeam, E::Air, T::Skylander, "Whirlwind (Eon's Elite)"},
    {1, 0x0000, G::SpyrosAdv, E::Air, T::Skylander, "Sonic Boom"},
    {1, 0x1801, G::Giants, E::Air, T::Skylander, "Sonic Boom (S2)"},
    {1, 0x1811, G::Giants, E::Air, T::Skylander, "Sonic Boom (Glow in the Dark)"},
    {1, 0x1813, G::Giants, E::Air, T::Skylander, "Sonic Boom (Sparkle)"},
    {2, 0x0000, G::SpyrosAdv, E::Air, T::Skylander, "Warnado"},
    {2, 0x2206, G::SwapForce, E::Air, T::Skylander, "Warnado (LightCore)"},
    {3, 0x0000, G::SpyrosAdv, E::Air, T::Skylander, "Lightning Rod"},
    {3, 0x1801, G::Giants, E::Air, T::Skylander, "Lightning Rod (S2)"},
    {3, 0x1814, G::Giants, E::Air, T::Skylander, "Lightning Rod (Metallic Purple)"},
    {4, 0x0000, G::SpyrosAdv, E::Earth, T::Skylander, "Bash"},
    {4, 0x1801, G::Giants, E::Earth, T::Skylander, "Bash (S2)"},
    {5, 0x0000, G::SpyrosAdv, E::Earth, T::Skylander, "Terrafin"},
    {5, 0x1801, G::Giants, E::Earth, T::Skylander, "Terrafin (S2)"},
    {5, 0x2805, G::SwapForce, E::Earth, T::Skylander, "Terrafin (Knockout)"},
    {5, 0x3810, G::TrapTeam, E::Earth, T::Skylander, "Terrafin (Eon's Elite)"},
    {6, 0x0000, G::SpyrosAdv, E::Earth, T::Skylander, "Dino-Rang"},
    {6, 0x4810, G::Superchargers, E::Earth, T::Skylander, "Dino-Rang (Eon's Elite)"},
    {7, 0x0000, G::SpyrosAdv, E::Earth, T::Skylander, "Prism Break"},
    {7, 0x1206, G::Giants, E::Earth, T::Skylander, "Prism Break (LightCore)"},
    {7, 0x1214, G::Giants, E::Earth, T::Skylander, "Prism Break (White Flocked LightCore)"},
    {7, 0x1801, G::Giants, E::Earth, T::Skylander, "Prism Break (S2)"},
    {7, 0x2805, G::SwapForce, E::Earth, T::Skylander, "Prism Break (Hyper Beam)"},
    {8, 0x0000, G::SpyrosAdv, E::Fire, T::Skylander, "Sunburn"},
    {9, 0x0000, G::SpyrosAdv, E::Fire, T::Skylander, "Eruptor"},
    {9, 0x1206, G::Giants, E::Fire, T::Skylander, "Eruptor (LightCore)"},
    {9, 0x1801, G::Giants, E::Fire, T::Skylander, "Eruptor (S2)"},
    {9, 0x1814, G::Giants, E::Fire, T::Skylander, "Eruptor (White Flocked)"},
    {9, 0x2805, G::SwapForce, E::Fire, T::Skylander, "Eruptor (Lava Barf)"},
    {9, 0x2C02, G::SwapForce, E::Fire, T::Skylander, "Eruptor (Volcanic Lava Barf)"},
    {9, 0x3810, G::TrapTeam, E::Fire, T::Skylander, "Eruptor (Eon's Elite)"},
    {10, 0x0000, G::SpyrosAdv, E::Fire, T::Skylander, "Ignitor"},
    {10, 0x1801, G::Giants, E::Fire, T::Skylander, "Ignitor (S2)"},
    {10, 0x1C03, G::Giants, E::Fire, T::Skylander, "Ignitor (Legendary)"},
    {11, 0x0000, G::SpyrosAdv, E::Fire, T::Skylander, "Flameslinger"},
    {11, 0x1801, G::Giants, E::Fire, T::Skylander, "Flameslinger (S2)"},
    {11, 0x1802, G::Giants, E::Fire, T::Skylander, "Flameslinger (Golden)"},
    {12, 0x0000, G::SpyrosAdv, E::Water, T::Skylander, "Zap"},
    {12, 0x1801, G::Giants, E::Water, T::Skylander, "Zap (S2)"},
    {13, 0x0000, G::SpyrosAdv, E::Water, T::Skylander, "Wham-Shell"},
    {13, 0x2206, G::SwapForce, E::Water, T::Skylander, "Wham-Shell (LightCore)"},
    {14, 0x0000, G::SpyrosAdv, E::Water, T::Skylander, "Gill Grunt"},
    {14, 0x1801, G::Giants, E::Water, T::Skylander, "Gill Grunt (S2)"},
    {14, 0x1817, G::Giants, E::Water, T::Skylander, "Gill Grunt (Metallic Green)"},
    {14, 0x2805, G::SwapForce, E::Water, T::Skylander, "Gill Grunt (Anchors Away)"},
    {14, 0x3809, G::TrapTeam, E::Water, T::Skylander, "Gill Grunt (Tidal Wave)"},
    {14, 0x3810, G::TrapTeam, E::Water, T::Skylander, "Gill Grunt (Eon's Elite)"},
    {15, 0x0000, G::SpyrosAdv, E::Water, T::Skylander, "Slam Bam"},
    {15, 0x1801, G::Giants, E::Water, T::Skylander, "Slam Bam (S2)"},
    {15, 0x1C03, G::Giants, E::Water, T::Skylander, "Slam Bam (Legendary)"},
    {15, 0x3810, G::Superchargers, E::Water, T::Skylander, "Slam Bam (Eon's Elite)"},
    {16, 0x0000, G::SpyrosAdv, E::Magic, T::Skylander, "Spyro"},
    {16, 0x1801, G::Giants, E::Magic, T::Skylander, "Spyro (S2)"},
    {16, 0x2805, G::SwapForce, E::Magic, T::Skylander, "Spyro (Mega Ram)"},
    {16, 0x2C02, G::SwapForce, E::Magic, T::Skylander, "Spyro (Dark Mega Ram)"},
    {16, 0x3810, G::TrapTeam, E::Magic, T::Skylander, "Spyro (Eon's Elite)"},
    {17, 0x0000, G::SpyrosAdv, E::Magic, T::Skylander, "Voodood"},
    {17, 0x3810, G::Superchargers, E::Magic, T::Skylander, "Voodood (Eon's Elite)"},
    {18, 0x0000, G::SpyrosAdv, E::Magic, T::Skylander, "Double Trouble"},
    {18, 0x1801, G::Giants, E::Magic, T::Skylander, "Double Trouble (S2)"},
    {18, 0x1C02, G::Giants, E::Magic, T::Skylander, "Double Trouble (Royal)"},
    {19, 0x0000, G::SpyrosAdv, E::Tech, T::Skylander, "Trigger Happy"},
    {19, 0x1801, G::Giants, E::Tech, T::Skylander, "Trigger Happy (S2)"},
    {19, 0x2805, G::SwapForce, E::Tech, T::Skylander, "Trigger Happy (Big Bang)"},
    {19, 0x2C02, G::SwapForce, E::Tech, T::Skylander, "Trigger Happy (Springtime Big Bang)"},
    {19, 0x3810, G::TrapTeam, E::Tech, T::Skylander, "Trigger Happy (Eon's Elite)"},
    {20, 0x0000, G::SpyrosAdv, E::Tech, T::Skylander, "Drobot"},
    {20, 0x1206, G::Giants, E::Tech, T::Skylander, "Drobot (LightCore)"},
    {20, 0x1801, G::Giants, E::Tech, T::Skylander, "Drobot (S2)"},
    {21, 0x0000, G::SpyrosAdv, E::Tech, T::Skylander, "Drill Sergeant"},
    {21, 0x1801, G::Giants, E::Tech, T::Skylander, "Drill Sergeant (S2)"},
    {22, 0x0000, G::SpyrosAdv, E::Tech, T::Skylander, "Boomer"},
    {22, 0x4810, G::Superchargers, E::Tech, T::Skylander, "Boomer (Eon's Elite)"},
    {23, 0x0000, G::SpyrosAdv, E::Magic, T::Skylander, "Wrecking Ball"},
    {23, 0x1801, G::Giants, E::Magic, T::Skylander, "Wrecking Ball (S2)"},
    {23, 0x1814, G::Giants, E::Magic, T::Skylander, "Wrecking Ball (Metallic Purple)"},
    {24, 0x0000, G::SpyrosAdv, E::Life, T::Skylander, "Camo"},
    {24, 0x2805, G::SwapForce, E::Life, T::Skylander, "Camo (Thorn Horn)"},
    {25, 0x0000, G::SpyrosAdv, E::Life, T::Skylander, "Zook"},
    {25, 0x1801, G::Giants, E::Life, T::Skylander, "Zook (S2)"},
    {25, 0x1812, G::Giants, E::Life, T::Skylander, "Zook (Stone)"},
    {25, 0x3810, G::Superchargers, E::Life, T::Skylander, "Zook (Eon's Elite)"},
    {26, 0x0000, G::SpyrosAdv, E::Life, T::Skylander, "Stealth Elf"},
    {26, 0x1801, G::Giants, E::Life, T::Skylander, "Stealth Elf (S2)"},
    {26, 0x1C03, G::Giants, E::Life, T::Skylander, "Stealth Elf (Legendary)"},
    {26, 0x2805, G::SwapForce, E::Life, T::Skylander, "Stealth Elf (Ninja)"},
    {26, 0x2C02, G::SwapForce, E::Life, T::Skylander, "Stealth Elf (Dark Ninja)"},
    {26, 0x3810, G::TrapTeam, E::Life, T::Skylander, "Stealth Elf (Eon's Elite)"},
    {27, 0x0000, G::SpyrosAdv, E::Life, T::Skylander, "Stump Smash"},
    {27, 0x1801, G::Giants, E::Life, T::Skylander, "Stump Smash (S2)"},
    {27, 0x1814, G::Giants, E::Life, T::Skylander, "Stump Smash (White Flocked)"},
    {28, 0x0000, G::SpyrosAdv, E::Magic, T::Skylander, "Spyro (Dark)"},
    {29, 0x0000, G::SpyrosAdv, E::Undead, T::Skylander, "Hex"},
    {29, 0x1206, G::Giants, E::Undead, T::Skylander, "Hex (LightCore)"},
    {29, 0x1801, G::Giants, E::Undead, T::Skylander, "Hex (S2)"},
    {30, 0x0000, G::SpyrosAdv, E::Undead, T::Skylander, "Chop Chop"},
    {30, 0x1801, G::Giants, E::Undead, T::Skylander, "Chop Chop (S2)"},
    {30, 0x1804, G::Giants, E::Undead, T::Skylander, "Chop Chop (Metallic Blue)"},
    {30, 0x2805, G::SwapForce, E::Undead, T::Skylander, "Chop Chop (Twin Blade)"},
    {30, 0x2816, G::SwapForce, E::Undead, T::Skylander, "Chop Chop (Green Twin Blade)"},
    {30, 0x3810, G::TrapTeam, E::Undead, T::Skylander, "Chop Chop (Eon's Elite)"},
    {31, 0x0000, G::SpyrosAdv, E::Undead, T::Skylander, "Ghost Roaster"},
    {31, 0x4810, G::Superchargers, E::Undead, T::Skylander, "Ghost Roaster (Eon's Elite)"},
    {32, 0x0000, G::SpyrosAdv, E::Undead, T::Skylander, "Cynder"},
    {32, 0x1801, G::Giants, E::Undead, T::Skylander, "Cynder (S2)"},
    {32, 0x1811, G::Giants, E::Undead, T::Skylander, "Cynder (Glow in the Dark)"},
    {32, 0x2805, G::SwapForce, E::Undead, T::Skylander, "Cynder (Phantom)"},
    {100, 0x1000, G::Giants, E::Air, T::Skylander, "Jet-Vac"},
    {100, 0x1206, G::Giants, E::Air, T::Skylander, "Jet-Vac (LightCore)"},
    {100, 0x1403, G::Giants, E::Air, T::Skylander, "Jet-Vac (Legendary)"},
    {100, 0x2805, G::SwapForce, E::Air, T::Skylander, "Jet-Vac (Turbo)"},
    {100, 0x3805, G::TrapTeam, E::Air, T::Skylander, "Jet-Vac (Full Blast)"},
    {101, 0x1206, G::Giants, E::Air, T::Giant, "Swarm"},
    {102, 0x1206, G::Giants, E::Earth, T::Giant, "Crusher"},
    {102, 0x1602, G::Giants, E::Earth, T::Giant, "Crusher (Granite)"},
    {103, 0x1000, G::Giants, E::Earth, T::Skylander, "Flashwing"},
    {103, 0x1402, G::Giants, E::Earth, T::Skylander, "Flashwing (Jade)"},
    {103, 0x2206, G::SwapForce, E::Earth, T::Skylander, "Flashwing (LightCore)"},
    {104, 0x1206, G::Giants, E::Fire, T::Giant, "Hot Head"},
    {104, 0x1213, G::Giants, E::Fire, T::Giant, "Hot Head (Sparkle)"},
    {105, 0x1000, G::Giants, E::Fire, T::Skylander, "Hot Dog"},
    {105, 0x1015, G::Giants, E::Fire, T::Skylander, "Hot Dog (Bronze)"},
    {105, 0x1402, G::Giants, E::Fire, T::Skylander, "Hot Dog (Molten)"},
    {105, 0x2805, G::SwapForce, E::Fire, T::Skylander, "Hot Dog (Fire Bone)"},
    {105, 0x281A, G::SwapForce, E::Fire, T::Skylander, "Hot Dog (Red Flame Frito-Lay Fire Bone)"},
    {105, 0x281B, G::SwapForce, E::Fire, T::Skylander, "Hot Dog (Green Flame Frito-Lay Fire Bone)"},
    {105, 0x281C, G::SwapForce, E::Fire, T::Skylander,
     "Hot Dog (Purple Flame Frito-Lay Fire Bone)"},
    {106, 0x1000, G::Giants, E::Water, T::Skylander, "Chill"},
    {106, 0x1206, G::Giants, E::Water, T::Skylander, "Chill (LightCore)"},
    {106, 0x1603, G::Giants, E::Water, T::Skylander, "Chill (Legendary LightCore)"},
    {106, 0x2805, G::SwapForce, E::Water, T::Skylander, "Chill (Blizzard)"},
    {107, 0x1206, G::Giants, E::Water, T::Giant, "Thumpback"},
    {108, 0x1000, G::Giants, E::Magic, T::Skylander, "Pop Fizz"},
    {108, 0x1206, G::Giants, E::Magic, T::Skylander, "Pop Fizz (LightCore)"},
    {108, 0x1402, G::Giants, E::Magic, T::Skylander, "Pop Fizz (Punch)"},
    {108, 0x2805, G::SwapForce, E::Magic, T::Skylander, "Pop Fizz (Super Gulp)"},
    {108, 0x3805, G::TrapTeam, E::Magic, T::Skylander, "Pop Fizz (Fizzy Frenzy)"},
    {108, 0x3C02, G::TrapTeam, E::Magic, T::Skylander, "Pop Fizz (Love Potion Fizzy Frenzy)"},
    {109, 0x1206, G::Giants, E::Magic, T::Giant, "Ninjini"},
    {109, 0x1602, G::Giants, E::Magic, T::Giant, "Ninjini (Scarlet)"},
    {110, 0x1206, G::Giants, E::Tech, T::Giant, "Bouncer"},
    {110, 0x1603, G::Giants, E::Tech, T::Giant, "Bouncer (Legendary)"},
    {111, 0x1000, G::Giants, E::Tech, T::Skylander, "Sprocket"},
    {111, 0x1013, G::Giants, E::Tech, T::Skylander, "Sprocket (Sparkle)"},
    {111, 0x2805, G::SwapForce, E::Tech, T::Skylander, "Sprocket (Heavy Duty)"},
    {111, 0x2819, G::SwapForce, E::Tech, T::Skylander, "Sprocket (Heavy Metal Heavy Duty)"},
    {112, 0x1206, G::Giants, E::Life, T::Giant, "Tree Rex"},
    {112, 0x1602, G::Giants, E::Life, T::Giant, "Tree Rex (Gnarly)"},
    {113, 0x1000, G::Giants, E::Life, T::Skylander, "Shroomboom"},
    {113, 0x1017, G::Giants, E::Life, T::Skylander, "Shroomboom (Metallic Green)"},
    {113, 0x1206, G::Giants, E::Life, T::Skylander, "Shroomboom (LightCore)"},
    {113, 0x3801, G::TrapTeam, E::Life, T::Skylander, "Shroomboom (Sure Shot)"},
    {114, 0x1206, G::Giants, E::Undead, T::Giant, "Eye-Brawl"},
    {114, 0x1214, G::Giants, E::Undead, T::Giant, "Eye-Brawl (Metallic Purple)"},
    {114, 0x1215, G::Giants, E::Undead, T::Giant, "Eye-Brawl (Pumpkin)"},
    {115, 0x1000, G::Giants, E::Undead, T::Skylander, "Fright Rider"},
    {115, 0x1011, G::Giants, E::Undead, T::Skylander, "Fright Rider (Glow in the Dark)"},
    {115, 0x1015, G::Giants, E::Undead, T::Skylander, "Fright Rider (Halloween)"},
    {200, 0x0000, G::SpyrosAdv, E::Other, T::Item, "Anvil Rain"},
    {201, 0x0000, G::SpyrosAdv, E::Other, T::Item, "Hidden Treasure"},
    {201, 0x0002, G::Giants, E::Other, T::Item, "Hidden Treasure (Platinum)"},
    {202, 0x0000, G::SpyrosAdv, E::Other, T::Item, "Healing Elixir"},
    {203, 0x0000, G::SpyrosAdv, E::Other, T::Item, "Ghost Pirate Swords"},
    {204, 0x0000, G::SpyrosAdv, E::Other, T::Item, "Time Twister Hourglass"},
    {205, 0x0000, G::SpyrosAdv, E::Other, T::Item, "Sky-Iron Shield"},
    {206, 0x0000, G::SpyrosAdv, E::Other, T::Item, "Winged Boots"},
    {207, 0x0000, G::SpyrosAdv, E::Other, T::Item, "Sparx Dragonfly"},
    {208, 0x1206, G::Giants, E::Other, T::Item, "Dragonfire Cannon"},
    {208, 0x1602, G::Giants, E::Other, T::Item, "Golden Dragonfire Cannon"},
    {209, 0x1206, G::Giants, E::Other, T::Item, "Scorpion Striker Catapult"},
    {210, 0x3002, G::TrapTeam, E::Magic, T::Trap, "Magic Log Holder"},
    {210, 0x3008, G::TrapTeam, E::Magic, T::Trap, "Magic Skull"},
    {210, 0x300B, G::TrapTeam, E::Magic, T::Trap, "Magic Axe"},
    {210, 0x300E, G::TrapTeam, E::Magic, T::Trap, "Magic Hourglass"},
    {210, 0x3012, G::TrapTeam, E::Magic, T::Trap, "Magic Totem"},
    {210, 0x3015, G::TrapTeam, E::Magic, T::Trap, "Magic Rocket"},
    {211, 0x3001, G::TrapTeam, E::Water, T::Trap, "Water Tiki"},
    {211, 0x3002, G::TrapTeam, E::Water, T::Trap, "Water Log Holder"},
    {211, 0x3006, G::TrapTeam, E::Water, T::Trap, "Water Jughead"},
    {211, 0x3007, G::TrapTeam, E::Water, T::Trap, "Water Angel"},
    {211, 0x300B, G::TrapTeam, E::Water, T::Trap, "Water Axe"},
    {211, 0x3016, G::TrapTeam, E::Water, T::Trap, "Water Flying Helmet"},
    {211, 0x3406, G::TrapTeam, E::Water, T::Trap, "Water Jughead (Legendary)"},
    {212, 0x3003, G::TrapTeam, E::Air, T::Trap, "Air Toucan"},
    {212, 0x3006, G::TrapTeam, E::Air, T::Trap, "Air Jughead"},
    {212, 0x300E, G::TrapTeam, E::Air, T::Trap, "Air Hourglass"},
    {212, 0x3010, G::TrapTeam, E::Air, T::Trap, "Air Snake"},
    {212, 0x3011, G::TrapTeam, E::Air, T::Trap, "Air Screamer"},
    {212, 0x3018, G::TrapTeam, E::Air, T::Trap, "Air Sword"},
    {213, 0x3004, G::TrapTeam, E::Undead, T::Trap, "Undead Orb"},
    {213, 0x3008, G::TrapTeam, E::Undead, T::Trap, "Undead Skull"},
    {213, 0x300B, G::TrapTeam, E::Undead, T::Trap, "Undead Axe"},
    {213, 0x300C, G::TrapTeam, E::Undead, T::Trap, "Undead Hand"},
    {213, 0x3010, G::TrapTeam, E::Undead, T::Trap, "Undead Snake"},
    {213, 0x3017, G::TrapTeam, E::Undead, T::Trap, "Undead Captain's Hat"},
    {213, 0x3404, G::TrapTeam, E::Undead, T::Trap, "Undead Orb (Legendary)"},
    {213, 0x3408, G::TrapTeam, E::Undead, T::Trap, "Undead Skull (Legendary)"},
    {214, 0x3001, G::TrapTeam, E::Tech, T::Trap, "Tech Tiki"},
    {214, 0x3007, G::TrapTeam, E::Tech, T::Trap, "Tech Angel"},
    {214, 0x3009, G::TrapTeam, E::Tech, T::Trap, "Tech Scepter"},
    {214, 0x300C, G::TrapTeam, E::Tech, T::Trap, "Tech Hand"},
    {214, 0x3016, G::TrapTeam, E::Tech, T::Trap, "Tech Flying Helmet"},
    {214, 0x301A, G::TrapTeam, E::Tech, T::Trap, "Tech Handstand"},
    {215, 0x3005, G::TrapTeam, E::Fire, T::Trap, "Fire Torch"},
    {215, 0x3009, G::TrapTeam, E::Fire, T::Trap, "Fire Scepter"},
    {215, 0x3011, G::TrapTeam, E::Fire, T::Trap, "Fire Screamer"},
    {215, 0x3012, G::TrapTeam, E::Fire, T::Trap, "Fire Totem"},
    {215, 0x3017, G::TrapTeam, E::Fire, T::Trap, "Fire Captain's Hat"},
    {215, 0x301B, G::TrapTeam, E::Fire, T::Trap, "Fire Yawn"},
    {216, 0x3003, G::TrapTeam, E::Earth, T::Trap, "Earth Toucan"},
    {216, 0x3004, G::TrapTeam, E::Earth, T::Trap, "Earth Orb"},
    {216, 0x300A, G::TrapTeam, E::Earth, T::Trap, "Earth Hammer"},
    {216, 0x300E, G::TrapTeam, E::Earth, T::Trap, "Earth Hourglass"},
    {216, 0x3012, G::TrapTeam, E::Earth, T::Trap, "Earth Totem"},
    {216, 0x301A, G::TrapTeam, E::Earth, T::Trap, "Earth Handstand"},
    {217, 0x3001, G::TrapTeam, E::Life, T::Trap, "Life Toucan"},
    {217, 0x3005, G::TrapTeam, E::Life, T::Trap, "Life Torch"},
    {217, 0x300A, G::TrapTeam, E::Life, T::Trap, "Life Hammer"},
    {217, 0x3010, G::TrapTeam, E::Life, T::Trap, "Life Snake"},
    {217, 0x3018, G::TrapTeam, E::Life, T::Trap, "Life Sword"},
    {217, 0x301B, G::TrapTeam, E::Life, T::Trap, "Life Yawn"},
    {218, 0x3014, G::TrapTeam, E::Dark, T::Trap, "Dark Spider"},
    {218, 0x3018, G::TrapTeam, E::Dark, T::Trap, "Dark Sword"},
    {218, 0x301A, G::TrapTeam, E::Dark, T::Trap, "Dark Handstand"},
    {219, 0x300F, G::TrapTeam, E::Light, T::Trap, "Light Owl"},
    {219, 0x3015, G::TrapTeam, E::Light, T::Trap, "Light Rocket"},
    {219, 0x301B, G::TrapTeam, E::Light, T::Trap, "Light Yawn"},
    {220, 0x301E, G::TrapTeam, E::Other, T::Trap, "Kaos"},
    {220, 0x351F, G::TrapTeam, E::Other, T::Trap, "Kaos (Ultimate)"},
    {230, 0x3000, G::TrapTeam, E::Other, T::Item, "Hand of Fate"},
    {230, 0x3403, G::TrapTeam, E::Other, T::Item, "Hand of Fate (Legendary)"},
    {231, 0x3000, G::TrapTeam, E::Other, T::Item, "Piggy Bank"},
    {232, 0x3000, G::TrapTeam, E::Other, T::Item, "Rocket Ram"},
    {233, 0x3000, G::TrapTeam, E::Other, T::Item, "Tiki Speaky"},
    {300, 0x0000, G::SpyrosAdv, E::Other, T::Item, "Dragon's Peak"},
    {301, 0x0000, G::SpyrosAdv, E::Other, T::Item, "Empire of Ice"},
    {302, 0x0000, G::SpyrosAdv, E::Other, T::Item, "Pirate Seas"},
    {303, 0x0000, G::SpyrosAdv, E::Other, T::Item, "Darklight Crypt"},
    {304, 0x0000, G::SpyrosAdv, E::Other, T::Item, "Volcanic Vault"},
    {305, 0x3000, G::TrapTeam, E::Other, T::Item, "Mirror of Mystery"},
    {306, 0x3000, G::TrapTeam, E::Other, T::Item, "Nightmare Express"},
    {307, 0x3206, G::TrapTeam, E::Other, T::Item, "Sunscraper Spire"},
    {308, 0x3206, G::TrapTeam, E::Other, T::Item, "Midnight Museum"},
    {404, 0x0000, G::SpyrosAdv, E::Earth, T::Skylander, "Bash (Legendary)"},
    {416, 0x0000, G::SpyrosAdv, E::Magic, T::Skylander, "Spyro (Legendary)"},
    {419, 0x0000, G::SpyrosAdv, E::Tech, T::Skylander, "Trigger Happy (Legendary)"},
    {430, 0x0000, G::SpyrosAdv, E::Undead, T::Skylander, "Chop Chop (Legendary)"},
    {450, 0x3000, G::TrapTeam, E::Air, T::TrapMaster, "Gusto"},
    {451, 0x3000, G::TrapTeam, E::Air, T::TrapMaster, "Thunderbolt"},
    {451, 0x301D, G::TrapTeam, E::Air, T::TrapMaster, "Thunderbolt (Clear)"},
    {452, 0x3000, G::TrapTeam, E::Air, T::Skylander, "Fling Kong"},
    {453, 0x3000, G::TrapTeam, E::Air, T::Skylander, "Blades"},
    {453, 0x3403, G::TrapTeam, E::Air, T::Skylander, "Blades (Legendary)"},
    {454, 0x3000, G::TrapTeam, E::Earth, T::TrapMaster, "Wallop"},
    {455, 0x3000, G::TrapTeam, E::Earth, T::TrapMaster, "Head Rush"},
    {455, 0x3402, G::TrapTeam, E::Earth, T::TrapMaster, "Head Rush (Nitro)"},
    {456, 0x3000, G::TrapTeam, E::Earth, T::Skylander, "Fist Bump"},
    {457, 0x3000, G::TrapTeam, E::Earth, T::Skylander, "Rocky Roll"},
    {458, 0x3000, G::TrapTeam, E::Fire, T::TrapMaster, "Wildfire"},
    {458, 0x3402, G::TrapTeam, E::Fire, T::TrapMaster, "Wildfire (Dark)"},
    {459, 0x3000, G::TrapTeam, E::Fire, T::TrapMaster, "Ka-Boom"},
    {460, 0x3000, G::TrapTeam, E::Fire, T::Skylander, "Trail Blazer"},
    {461, 0x3000, G::TrapTeam, E::Fire, T::Skylander, "Torch"},
    {462, 0x3000, G::TrapTeam, E::Water, T::TrapMaster, "Snap Shot"},
    {462, 0x3402, G::TrapTeam, E::Water, T::TrapMaster, "Snap Shot (Dark)"},
    {462, 0x450F, G::TrapTeam, E::Water, T::TrapMaster, "Snap Shot (Virtual)"},
    {463, 0x3000, G::TrapTeam, E::Water, T::TrapMaster, "Lob-Star"},
    {463, 0x3402, G::TrapTeam, E::Water, T::TrapMaster, "Lob-Star (Winterfest)"},
    {464, 0x3000, G::TrapTeam, E::Water, T::Skylander, "Flip Wreck"},
    {465, 0x3000, G::TrapTeam, E::Water, T::Skylander, "Echo"},
    {466, 0x3000, G::TrapTeam, E::Magic, T::TrapMaster, "Blastermind"},
    {467, 0x3000, G::TrapTeam, E::Magic, T::TrapMaster, "Enigma"},
    {468, 0x3000, G::TrapTeam, E::Magic, T::Skylander, "Deja Vu"},
    {468, 0x3403, G::TrapTeam, E::Magic, T::Skylander, "Deja Vu (Legendary)"},
    {469, 0x3000, G::TrapTeam, E::Magic, T::Skylander, "Cobra Cadabra"},
    {469, 0x3402, G::TrapTeam, E::Magic, T::Skylander, "Cobra Cadabra (King)"},
    {470, 0x3000, G::TrapTeam, E::Tech, T::TrapMaster, "Jawbreaker"},
    {470, 0x3403, G::TrapTeam, E::Tech, T::TrapMaster, "Jawbreaker (Legendary)"},
    {471, 0x3000, G::TrapTeam, E::Tech, T::TrapMaster, "Gearshift"},
    {472, 0x3000, G::TrapTeam, E::Tech, T::Skylander, "Chopper"},
    {473, 0x3000, G::TrapTeam, E::Tech, T::Skylander, "Tread Head"},
    {474, 0x3000, G::TrapTeam, E::Life, T::TrapMaster, "Bushwhack"},
    {474, 0x3403, G::TrapTeam, E::Life, T::TrapMaster, "Bushwhack (Legendary)"},
    {475, 0x3000, G::TrapTeam, E::Life, T::TrapMaster, "Tuff Luck"},
    {475, 0x301D, G::TrapTeam, E::Life, T::TrapMaster, "Tuff Luck (Clear)"},
    {476, 0x3000, G::TrapTeam, E::Life, T::Skylander, "Food Fight"},
    {476, 0x3402, G::TrapTeam, E::Life, T::Skylander, "Food Fight (Dark)"},
    {476, 0x450F, G::TrapTeam, E::Life, T::Skylander, "Food Fight (Virtual)"},
    {477, 0x3000, G::TrapTeam, E::Life, T::Skylander, "High Five"},
    {478, 0x3000, G::TrapTeam, E::Undead, T::TrapMaster, "Krypt King"},
    {478, 0x3402, G::TrapTeam, E::Undead, T::TrapMaster, "Krypt King (Nitro)"},
    {479, 0x3000, G::TrapTeam, E::Undead, T::TrapMaster, "Short Cut"},
    {479, 0x301D, G::TrapTeam, E::Undead, T::TrapMaster, "Short Cut (Clear)"},
    {480, 0x3000, G::TrapTeam, E::Undead, T::Skylander, "Bat Spin"},
    {481, 0x3000, G::TrapTeam, E::Undead, T::Skylander, "Funny Bone"},
    {482, 0x3000, G::TrapTeam, E::Light, T::TrapMaster, "Knight Light"},
    {483, 0x3000, G::TrapTeam, E::Light, T::Skylander, "Spotlight"},
    {484, 0x3000, G::TrapTeam, E::Dark, T::TrapMaster, "Knight Mare"},
    {485, 0x3000, G::TrapTeam, E::Dark, T::Skylander, "Blackout"},
    {502, 0x3000, G::TrapTeam, E::Earth, T::Mini, "Bop"},
    {503, 0x3000, G::TrapTeam, E::Magic, T::Mini, "Spry"},
    {504, 0x3000, G::TrapTeam, E::Undead, T::Mini, "Hijinx"},
    {505, 0x0000, G::SpyrosAdv, E::Earth, T::Mini, "Terrabite (Sidekick)"},
    {505, 0x3000, G::TrapTeam, E::Earth, T::Mini, "Terrabite"},
    {506, 0x3000, G::TrapTeam, E::Air, T::Mini, "Breeze"},
    {507, 0x3000, G::TrapTeam, E::Fire, T::Mini, "Weeruptor"},
    {507, 0x3402, G::TrapTeam, E::Fire, T::Mini, "Weeruptor (Eggsellent)"},
    {508, 0x3000, G::TrapTeam, E::Air, T::Mini, "Pet Vac"},
    {508, 0x3402, G::TrapTeam, E::Air, T::Mini, "Pet Vac (Power Punch)"},
    {509, 0x3000, G::TrapTeam, E::Fire, T::Mini, "Small Fry"},
    {510, 0x3000, G::TrapTeam, E::Tech, T::Mini, "Drobit"},
    {514, 0x0000, G::SpyrosAdv, E::Water, T::Mini, "Gill Runt (Sidekick)"},
    {514, 0x3000, G::TrapTeam, E::Water, T::Mini, "Gill Runt"},
    {519, 0x0000, G::SpyrosAdv, E::Tech, T::Mini, "Trigger Snappy (Sidekick)"},
    {519, 0x3000, G::TrapTeam, E::Tech, T::Mini, "Trigger Snappy"},
    {526, 0x0000, G::SpyrosAdv, E::Life, T::Mini, "Whisper Elf (Sidekick)"},
    {526, 0x3000, G::TrapTeam, E::Life, T::Mini, "Whisper Elf"},
    {540, 0x1000, G::Giants, E::Life, T::Mini, "Barkley (Sidekick)"},
    {540, 0x3000, G::TrapTeam, E::Life, T::Mini, "Barkley"},
    {540, 0x3402, G::TrapTeam, E::Life, T::Mini, "Barkley (Gnarly)"},
    {541, 0x1000, G::Giants, E::Water, T::Mini, "Thumpling (Sidekick)"},
    {541, 0x3000, G::TrapTeam, E::Water, T::Mini, "Thumpling"},
    {542, 0x1000, G::Giants, E::Magic, T::Mini, "Mini Jini (Sidekick)"},
    {542, 0x3000, G::TrapTeam, E::Magic, T::Mini, "Mini Jini"},
    {543, 0x1000, G::Giants, E::Undead, T::Mini, "Eye-Small (Sidekick)"},
    {543, 0x3000, G::TrapTeam, E::Undead, T::Mini, "Eye-Small"},
    {1000, 0x2000, G::SwapForce, E::Air, T::Swapper, "Boom Jet (Bottom)"},
    {1001, 0x2000, G::SwapForce, E::Air, T::Swapper, "Free Ranger (Bottom)"},
    {1001, 0x2403, G::SwapForce, E::Air, T::Swapper, "Free Ranger (Legendary) (Bottom)"},
    {1002, 0x2000, G::SwapForce, E::Earth, T::Swapper, "Rubble Rouser (Bottom)"},
    {1003, 0x2000, G::SwapForce, E::Earth, T::Swapper, "Doom Stone (Bottom)"},
    {1003, 0x2016, G::SwapForce, E::Earth, T::Swapper, "Doom Stone (Gold & Bronze) (Bottom)"},
    {1004, 0x2000, G::SwapForce, E::Fire, T::Swapper, "Blast Zone (Bottom)"},
    {1004, 0x2402, G::SwapForce, E::Fire, T::Swapper, "Blast Zone (Dark) (Bottom)"},
    {1005, 0x2000, G::SwapForce, E::Fire, T::Swapper, "Fire Kraken (Bottom)"},
    {1005, 0x2004, G::SwapForce, E::Fire, T::Swapper, "Fire Kraken (Gold) (Bottom)"},
    {1005, 0x2402, G::SwapForce, E::Fire, T::Swapper, "Fire Kraken (Jade) (Bottom)"},
    {1006, 0x2000, G::SwapForce, E::Life, T::Swapper, "Stink Bomb (Bottom)"},
    {1006, 0x2016, G::SwapForce, E::Life, T::Swapper, "Stink Bomb (Silver & Gold) (Bottom)"},
    {1007, 0x2000, G::SwapForce, E::Life, T::Swapper, "Grilla Drilla (Bottom)"},
    {1008, 0x2000, G::SwapForce, E::Magic, T::Swapper, "Hoot Loop (Bottom)"},
    {1008, 0x2402, G::SwapForce, E::Magic, T::Swapper, "Hoot Loop (Enchanted) (Bottom)"},
    {1009, 0x2000, G::SwapForce, E::Magic, T::Swapper, "Trap Shadow (Bottom)"},
    {1009, 0x2016, G::SwapForce, E::Magic, T::Swapper, "Trap Shadow (Bronze & Silver) (Bottom)"},
    {1010, 0x2000, G::SwapForce, E::Tech, T::Swapper, "Magna Charge (Bottom)"},
    {1010, 0x2402, G::SwapForce, E::Tech, T::Swapper, "Magna Charge (Nitro) (Bottom)"},
    {1011, 0x2000, G::SwapForce, E::Tech, T::Swapper, "Spy Rise (Bottom)"},
    {1012, 0x2000, G::SwapForce, E::Undead, T::Swapper, "Night Shift (Bottom)"},
    {1012, 0x2403, G::SwapForce, E::Undead, T::Swapper, "Night Shift (Legendary) (Bottom)"},
    {1013, 0x2000, G::SwapForce, E::Undead, T::Swapper, "Rattle Shake (Bottom)"},
    {1013, 0x2402, G::SwapForce, E::Undead, T::Swapper, "Rattle Shake (Quickdraw) (Bottom)"},
    {1014, 0x2000, G::SwapForce, E::Water, T::Swapper, "Freeze Blade (Bottom)"},
    {1014, 0x2402, G::SwapForce, E::Water, T::Swapper, "Freeze Blade (Nitro) (Bottom)"},
    {1015, 0x2000, G::SwapForce, E::Water, T::Swapper, "Wash Buckler (Bottom)"},
    {1015, 0x2018, G::SwapForce, E::Water, T::Swapper, "Wash Buckler (Gold) (Bottom)"},
    {1015, 0x2402, G::SwapForce, E::Water, T::Swapper, "Wash Buckler (Dark) (Bottom)"},
    {2000, 0x2000, G::SwapForce, E::Air, T::Swapper, "Boom Jet (Top)"},
    {2001, 0x2000, G::SwapForce, E::Air, T::Swapper, "Free Ranger (Top)"},
    {2001, 0x2403, G::SwapForce, E::Air, T::Swapper, "Free Ranger (Legendary) (Top)"},
    {2002, 0x2000, G::SwapForce, E::Earth, T::Swapper, "Rubble Rouser (Top)"},
    {2003, 0x2000, G::SwapForce, E::Earth, T::Swapper, "Doom Stone (Top)"},
    {2003, 0x2016, G::SwapForce, E::Earth, T::Swapper, "Doom Stone (Gold & Bronze) (Top)"},
    {2004, 0x2000, G::SwapForce, E::Fire, T::Swapper, "Blast Zone (Top)"},
    {2004, 0x2402, G::SwapForce, E::Fire, T::Swapper, "Blast Zone (Dark) (Top)"},
    {2005, 0x2000, G::SwapForce, E::Fire, T::Swapper, "Fire Kraken (Top)"},
    {2005, 0x2004, G::SwapForce, E::Fire, T::Swapper, "Fire Kraken (Gold) (Top)"},
    {2005, 0x2402, G::SwapForce, E::Fire, T::Swapper, "Fire Kraken (Jade) (Top)"},
    {2006, 0x2000, G::SwapForce, E::Life, T::Swapper, "Stink Bomb (Top)"},
    {2006, 0x2016, G::SwapForce, E::Life, T::Swapper, "Stink Bomb (Silver & Gold) (Top)"},
    {2007, 0x2000, G::SwapForce, E::Life, T::Swapper, "Grilla Drilla (Top)"},
    {2008, 0x2000, G::SwapForce, E::Magic, T::Swapper, "Hoot Loop (Top)"},
    {2008, 0x2402, G::SwapForce, E::Magic, T::Swapper, "Hoot Loop (Enchanted) (Top)"},
    {2009, 0x2000, G::SwapForce, E::Magic, T::Swapper, "Trap Shadow (Top)"},
    {2009, 0x2016, G::SwapForce, E::Magic, T::Swapper, "Trap Shadow (Bronze & Silver) (Top)"},
    {2010, 0x2000, G::SwapForce, E::Tech, T::Swapper, "Magna Charge (Top)"},
    {2010, 0x2402, G::SwapForce, E::Tech, T::Swapper, "Magna Charge (Nitro) (Top)"},
    {2011, 0x2000, G::SwapForce, E::Tech, T::Swapper, "Spy Rise (Top)"},
    {2012, 0x2000, G::SwapForce, E::Undead, T::Swapper, "Night Shift (Top)"},
    {2012, 0x2403, G::SwapForce, E::Undead, T::Swapper, "Night Shift (Legendary) (Top)"},
    {2013, 0x2000, G::SwapForce, E::Undead, T::Swapper, "Rattle Shake (Top)"},
    {2013, 0x2402, G::SwapForce, E::Undead, T::Swapper, "Rattle Shake (Quickdraw) (Top)"},
    {2014, 0x2000, G::SwapForce, E::Water, T::Swapper, "Freeze Blade (Top)"},
    {2014, 0x2402, G::SwapForce, E::Water, T::Swapper, "Freeze Blade (Nitro) (Top)"},
    {2015, 0x2000, G::SwapForce, E::Water, T::Swapper, "Wash Buckler (Top)"},
    {2015, 0x2018, G::SwapForce, E::Water, T::Swapper, "Wash Buckler (Gold) (Top)"},
    {2015, 0x2402, G::SwapForce, E::Water, T::Swapper, "Wash Buckler (Dark) (Top)"},
    {3000, 0x2000, G::SwapForce, E::Air, T::Skylander, "Scratch"},
    {3001, 0x2000, G::SwapForce, E::Air, T::Skylander, "Pop Thorn"},
    {3002, 0x2000, G::SwapForce, E::Earth, T::Skylander, "Slobber Tooth"},
    {3002, 0x2402, G::SwapForce, E::Earth, T::Skylander, "Slobber Tooth (Dark)"},
    {3003, 0x2000, G::SwapForce, E::Earth, T::Skylander, "Scorp"},
    {3003, 0x2016, G::SwapForce, E::Earth, T::Skylander, "Scorp (Green)"},
    {3004, 0x2000, G::SwapForce, E::Fire, T::Skylander, "Fryno"},
    {3004, 0x3801, G::TrapTeam, E::Fire, T::Skylander, "Fryno (Hog Wild)"},
    {3005, 0x2000, G::SwapForce, E::Fire, T::Skylander, "Smolderdash"},
    {3005, 0x2206, G::SwapForce, E::Fire, T::Skylander, "Smolderdash (LightCore)"},
    {3005, 0x2219, G::SwapForce, E::Fire, T::Skylander, "Snowderdash (LightCore)"},
    {3006, 0x2000, G::SwapForce, E::Life, T::Skylander, "Bumble Blast"},
    {3006, 0x2206, G::SwapForce, E::Life, T::Skylander, "Bumble Blast (LightCore)"},
    {3006, 0x2402, G::SwapForce, E::Life, T::Skylander, "Bumble Blast (Jolly)"},
    {3007, 0x2000, G::SwapForce, E::Life, T::Skylander, "Zoo Lou"},
    {3007, 0x2403, G::SwapForce, E::Life, T::Skylander, "Zoo Lou (Legendary)"},
    {3008, 0x2000, G::SwapForce, E::Magic, T::Skylander, "Dune Bug"},
    {3008, 0x2004, G::SwapForce, E::Magic, T::Skylander, "Dune Bug (Metallic Red)"},
    {3009, 0x2000, G::SwapForce, E::Magic, T::Skylander, "Star Strike"},
    {3009, 0x2206, G::SwapForce, E::Magic, T::Skylander, "Star Strike (LightCore)"},
    {3009, 0x2602, G::SwapForce, E::Magic, T::Skylander, "Star Strike (Enchanted LightCore)"},
    {3010, 0x2000, G::SwapForce, E::Tech, T::Skylander, "Countdown"},
    {3010, 0x2206, G::SwapForce, E::Tech, T::Skylander, "Countdown (LightCore)"},
    {3010, 0x2402, G::SwapForce, E::Tech, T::Skylander, "Countdown (Kickoff)"},
    {3011, 0x2000, G::SwapForce, E::Tech, T::Skylander, "Wind Up"},
    {3011, 0x2404, G::SwapForce, E::Tech, T::Skylander, "Wind Up (Gear Head Vicarious Visions)"},
    {3012, 0x2000, G::SwapForce, E::Undead, T::Skylander, "Roller Brawl"},
    {3013, 0x2000, G::SwapForce, E::Undead, T::Skylander, "Grim Creeper"},
    {3013, 0x2206, G::SwapForce, E::Undead, T::Skylander, "Grim Creeper (LightCore)"},
    {3013, 0x2603, G::SwapForce, E::Undead, T::Skylander, "Grim Creeper (Legendary LightCore)"},
    {3014, 0x2000, G::SwapForce, E::Water, T::Skylander, "Rip Tide"},
    {3014, 0x2016, G::SwapForce, E::Water, T::Skylander, "Rip Tide (Green)"},
    {3015, 0x2000, G::SwapForce, E::Water, T::Skylander, "Punk Shock"},
    {3200, 0x2000, G::SwapForce, E::Other, T::Item, "Battle Hammer"},
    {3201, 0x2000, G::SwapForce, E::Other, T::Item, "Sky Diamond"},
    {3202, 0x2000, G::SwapForce, E::Other, T::Item, "Platinum Sheep"},
    {3203, 0x2000, G::SwapForce, E::Other, T::Item, "Groove Machine"},
    {3204, 0x2000, G::SwapForce, E::Other, T::Item, "UFO Hat"},
    {3220, 0x4000, G::Superchargers, E::Air, T::Vehicle, "Jet Stream"},
    {3221, 0x4000, G::Superchargers, E::Undead, T::Vehicle, "Tomb Buggy"},
    {3221, 0x401E, G::Superchargers, E::Undead, T::Vehicle, "Tomb Buggy (Bronze)"},
    {3222, 0x4000, G::Superchargers, E::Water, T::Vehicle, "Reef Ripper"},
    {3223, 0x4000, G::Superchargers, E::Fire, T::Vehicle, "Burn-Cycle"},
    {3223, 0x401E, G::Superchargers, E::Fire, T::Vehicle, "Burn-Cycle (Patina)"},
    {3224, 0x4000, G::Superchargers, E::Fire, T::Vehicle, "Hot Streak"},
    {3224, 0x4004, G::Superchargers, E::Fire, T::Vehicle, "Hot Streak (Event Exclusive)"},
    {3224, 0x411F, G::Superchargers, E::Fire, T::Vehicle, "Hot Streak (Mobile)"},
    {3224, 0x4402, G::Superchargers, E::Fire, T::Vehicle, "Hot Streak (Dark)"},
    {3224, 0x441E, G::Superchargers, E::Fire, T::Vehicle, "Hot Streak (Golden)"},
    {3224, 0x450F, G::Superchargers, E::Fire, T::Vehicle, "Hot Streak (Instant)"},
    {3225, 0x4000, G::Superchargers, E::Earth, T::Vehicle, "Shark Tank"},
    {3226, 0x4000, G::Superchargers, E::Earth, T::Vehicle, "Thump Truck"},
    {3227, 0x4000, G::Superchargers, E::Undead, T::Vehicle, "Crypt Crusher"},
    {3228, 0x4000, G::Superchargers, E::Life, T::Vehicle, "Stealth Stinger"},
    {3228, 0x4402, G::Superchargers, E::Life, T::Vehicle, "Stealth Stinger (Nitro)"},
    {3228, 0x450F, G::Superchargers, E::Life, T::Vehicle, "Stealth Stinger (Instant)"},
    {3231, 0x4000, G::Superchargers, E::Water, T::Vehicle, "Dive Bomber"},
    {3231, 0x4402, G::Superchargers, E::Water, T::Vehicle, "Dive Bomber (Spring Ahead)"},
    {3231, 0x450F, G::Superchargers, E::Water, T::Vehicle, "Dive Bomber (Instant)"},
    {3232, 0x4000, G::Superchargers, E::Air, T::Vehicle, "Sky Slicer"},
    {3233, 0x4000, G::Superchargers, E::Air, T::Vehicle, "Clown Cruiser"},
    {3233, 0x4402, G::Superchargers, E::Air, T::Vehicle, "Clown Cruiser (Dark)"},
    {3234, 0x4000, G::Superchargers, E::Tech, T::Vehicle, "Gold Rusher"},
    {3234, 0x4402, G::Superchargers, E::Tech, T::Vehicle, "Gold Rusher (Power Blue)"},
    {3235, 0x4000, G::Superchargers, E::Tech, T::Vehicle, "Shield Striker"},
    {3235, 0x401E, G::Superchargers, E::Tech, T::Vehicle, "Shield Striker (Patina)"},
    {3236, 0x4000, G::Superchargers, E::Light, T::Vehicle, "Sun Runner"},
    {3236, 0x4403, G::Superchargers, E::Light, T::Vehicle, "Sun Runner (Legendary)"},
    {3237, 0x4000, G::Superchargers, E::Dark, T::Vehicle, "Sea Shadow"},
    {3237, 0x4402, G::Superchargers, E::Dark, T::Vehicle, "Sea Shadow (Dark)"},
    {3238, 0x4000, G::Superchargers, E::Magic, T::Vehicle, "Splatter Splasher"},
    {3238, 0x4402, G::Superchargers, E::Magic, T::Vehicle, "Splatter Splasher (Power Blue)"},
    {3239, 0x4000, G::Superchargers, E::Magic, T::Vehicle, "Soda Skimmer"},
    {3239, 0x4402, G::Superchargers, E::Magic, T::Vehicle, "Soda Skimmer (Nitro)"},
    {3240, 0x4000, G::Superchargers, E::Tech, T::Vehicle, "Barrel Blaster"},
    {3240, 0x4402, G::Superchargers, E::Tech, T::Vehicle, "Barrel Blaster (Dark)"},
    {3241, 0x4000, G::Superchargers, E::Life, T::Vehicle, "Buzz Wing"},
    {3300, 0x2000, G::SwapForce, E::Other, T::Item, "Sheep Wreck Island"},
    {3301, 0x2000, G::SwapForce, E::Other, T::Item, "Tower of Time"},
    {3302, 0x2206, G::SwapForce, E::Other, T::Item, "Fiery Forge"},
    {3303, 0x2206, G::SwapForce, E::Other, T::Item, "Arkeyan Crossbow"},
    {3400, 0x4100, G::Superchargers, E::Undead, T::Skylander, "Fiesta"},
    {3400, 0x4515, G::Superchargers, E::Undead, T::Skylander, "Fiesta (Frightful)"},
    {3401, 0x4100, G::Superchargers, E::Tech, T::Skylander, "High Volt"},
    {3401, 0x411E, G::Superchargers, E::Tech, T::Skylander, "High Volt (Patina)"},
    {3402, 0x4100, G::Superchargers, E::Magic, T::Skylander, "Splat"},
    {3402, 0x4502, G::Superchargers, E::Magic, T::Skylander, "Splat (Power Blue)"},
    {3406, 0x4100, G::Superchargers, E::Air, T::Skylander, "Stormblade"},
    {3406, 0x410E, G::Superchargers, E::Air, T::Skylander, "Stormblade (Snow-Brite)"},
    {3411, 0x4100, G::Superchargers, E::Earth, T::Skylander, "Smash Hit"},
    {3411, 0x4502, G::Superchargers, E::Earth, T::Skylander, "Smash Hit (Steel Plated)"},
    {3412, 0x4100, G::Superchargers, E::Fire, T::Skylander, "Spitfire"},
    {3412, 0x4502, G::Superchargers, E::Fire, T::Skylander, "Spitfire (Dark)"},
    {3412, 0x450F, G::Superchargers, E::Fire, T::Skylander, "Spitfire (Instant)"},
    {3413, 0x4100, G::Superchargers, E::Air, T::Skylander, "Jet-Vac (Hurricane)"},
    {3413, 0x4503, G::Superchargers, E::Air, T::Skylander, "Jet-Vac (Legendary Hurricane)"},
    {3414, 0x4100, G::Superchargers, E::Tech, T::Skylander, "Trigger Happy (Double Dare)"},
    {3414, 0x4502, G::Superchargers, E::Tech, T::Skylander,
     "Trigger Happy (Power Blue Double Dare)"},
    {3415, 0x4100, G::Superchargers, E::Life, T::Skylander, "Stealth Elf (Super Shot)"},
    {3415, 0x4502, G::Superchargers, E::Life, T::Skylander, "Stealth Elf (Dark Super Shot)"},
    {3415, 0x450F, G::Superchargers, E::Life, T::Skylander, "Stealth Elf (Instant Super Shot)"},
    {3416, 0x4100, G::Superchargers, E::Earth, T::Skylander, "Terrafin (Shark Shooter)"},
    {3417, 0x4100, G::Superchargers, E::Undead, T::Skylander, "Roller Brawl (Bone Bash)"},
    {3417, 0x411E, G::Superchargers, E::Undead, T::Skylander, "Roller Brawl (Bronze Bone Bash)"},
    {3417, 0x4503, G::Superchargers, E::Undead, T::Skylander, "Roller Brawl (Legendary Bone Bash)"},
    {3420, 0x4100, G::Superchargers, E::Magic, T::Skylander, "Pop Fizz (Big Bubble)"},
    {3420, 0x450E, G::Superchargers, E::Magic, T::Skylander, "Pop Fizz (Birthday Bash Big Bubble)"},
    {3421, 0x4100, G::Superchargers, E::Fire, T::Skylander, "Eruptor (Lava Lance)"},
    {3421, 0x411E, G::Superchargers, E::Fire, T::Skylander, "Eruptor (Patina Lava Lance)"},
    {3422, 0x4100, G::Superchargers, E::Water, T::Skylander, "Gill Grunt (Deep Dive)"},
    {3423, 0x4100, G::Superchargers, E::Life, T::Skylander, "Donkey Kong (Turbo Charge)"},
    {3423, 0x4502, G::Superchargers, E::Life, T::Skylander, "Donkey Kong (Dark Turbo Charge)"},
    {3424, 0x4100, G::Superchargers, E::Fire, T::Skylander, "Bowser (Hammer Slam)"},
    {3424, 0x4502, G::Superchargers, E::Fire, T::Skylander, "Bowser (Dark Hammer Slam)"},
    {3425, 0x4100, G::Superchargers, E::Water, T::Skylander, "Dive-Clops"},
    {3425, 0x450E, G::Superchargers, E::Water, T::Skylander, "Dive-Clops (Missile-Tow)"},
    {3425, 0x450F, G::Superchargers, E::Water, T::Skylander, "Dive-Clops (Instant)"},
    {3426, 0x4100, G::Superchargers, E::Tech, T::Skylander, "Astroblast"},
    {3426, 0x4503, G::Superchargers, E::Light, T::Skylander, "Astroblast (Legendary)"},
    {3427, 0x4100, G::Superchargers, E::Dark, T::Skylander, "Nightfall"},
    {3428, 0x4100, G::Superchargers, E::Life, T::Skylander, "Thrillipede"},
    {3428, 0x450D, G::Superchargers, E::Life, T::Skylander, "Thrillipede (Eggcited)"},
    {3500, 0x4000, G::Superchargers, E::Air, T::Trophy, "Sky Trophy"},
    {3501, 0x4000, G::Superchargers, E::Earth, T::Trophy, "Land Trophy"},
    {3502, 0x4000, G::Superchargers, E::Water, T::Trophy, "Sea Trophy"},
    {3503, 0x4000, G::Superchargers, E::Other, T::Trophy, "Kaos Trophy"},
};
// clang-format on

std::map<const std::pair<const u16, const u16>, SkyData> BuildFigureMap()
{
  std::map<const std::pair<const u16, const u16>, SkyData> figures;
  // The first entry of an ID pair is the one that counts.
  for (const FigureListEntry& entry : FIGURE_LIST)
  {
    figures.emplace(std::pair{entry.figure_id, entry.variant_id},
                    SkyData{entry.name, entry.game, entry.element, entry.type});
  }
  return figures;
}

// HID class request, host to device, to the interface.
constexpr u8 HID_SET_REQUEST_TYPE = 0x21;
constexpr u8 HID_SET_REPORT = 0x09;
constexpr u8 HID_SET_IDLE = 0x0A;
constexpr u8 HID_SET_PROTOCOL = 0x0B;

// Reply delays in microseconds.
constexpr u64 CONTROL_DELAY_US = 100;
constexpr u64 QUEUED_REPLY_DELAY_US = 22000;
constexpr u64 STATUS_DELAY_US = 2000;

// Interrupt transfers carry 32-byte reports; longer ones (up to 64 bytes) are speaker audio.
constexpr u32 REPORT_LENGTH = 32;
constexpr u32 MAX_AUDIO_LENGTH = 64;
}  // namespace

const std::map<const std::pair<const u16, const u16>, SkyData> list_skylanders = BuildFigureMap();

SkylanderUSB::SkylanderUSB()
{
  m_vid = 0x1430;
  m_pid = 0x0150;
  m_id = (u64{m_vid} << 32) | (u64{m_pid} << 16) | (u64{9} << 8) | u64{1};
  m_device_descriptor = DeviceDescriptor{0x12,   0x1,   0x200, 0x0, 0x0, 0x0, 0x40,
                                         0x1430, 0x150, 0x100, 0x1, 0x2, 0x0, 0x1};
  m_config_descriptor.emplace_back(ConfigDescriptor{0x9, 0x2, 0x29, 0x1, 0x1, 0x0, 0x80, 0xFA});
  m_interface_descriptor.emplace_back(
      InterfaceDescriptor{0x9, 0x4, 0x0, 0x0, 0x2, 0x3, 0x0, 0x0, 0x0});
  // Interrupt IN 0x81 and OUT 0x02, 64 bytes, every frame.
  m_endpoint_descriptor.emplace_back(EndpointDescriptor{0x7, 0x5, 0x81, 0x3, 0x40, 0x1});
  m_endpoint_descriptor.emplace_back(EndpointDescriptor{0x7, 0x5, 0x2, 0x3, 0x40, 0x1});
}

SkylanderUSB::~SkylanderUSB() = default;

DeviceDescriptor SkylanderUSB::GetDeviceDescriptor() const
{
  return m_device_descriptor;
}

std::vector<ConfigDescriptor> SkylanderUSB::GetConfigurations() const
{
  return m_config_descriptor;
}

std::vector<InterfaceDescriptor> SkylanderUSB::GetInterfaces(u8 /*config*/) const
{
  return m_interface_descriptor;
}

std::vector<EndpointDescriptor> SkylanderUSB::GetEndpoints(u8 /*config*/, u8 /*iface*/,
                                                           u8 /*alt*/) const
{
  return m_endpoint_descriptor;
}

bool SkylanderUSB::Attach()
{
  if (!m_device_attached)
  {
    DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x}] Opening device", m_vid, m_pid);
    m_device_attached = true;
  }
  return true;
}

bool SkylanderUSB::AttachAndChangeInterface(const u8 iface)
{
  if (!Attach())
    return false;
  return iface == m_active_interface || ChangeInterface(iface) == 0;
}

int SkylanderUSB::CancelTransfer(const u8 endpoint)
{
  INFO_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Cancelling transfers (endpoint {:#x})", m_vid, m_pid,
               m_active_interface, endpoint);
  return IPC_SUCCESS;
}

int SkylanderUSB::ChangeInterface(const u8 iface)
{
  DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Changing interface to {}", m_vid, m_pid,
                m_active_interface, iface);
  m_active_interface = iface;
  return 0;
}

int SkylanderUSB::GetNumberOfAltSettings(u8 /*iface*/)
{
  return 0;
}

int SkylanderUSB::SetAltSetting(u8 /*alt_setting*/)
{
  return 0;
}

// Each command has a fixed length; a command of another length is not accepted. The control
// transfer echoes the command (padded to its reply length) and some commands also queue a
// report for the interrupt endpoint.
auto SkylanderUSB::HandlePortalCommand(SkylanderPortal& portal, std::span<const u8> command)
    -> CommandReply
{
  CommandReply reply;
  const auto echo = [&](std::size_t count, s32 length) {
    std::copy_n(command.begin(), count, reply.control.begin());
    reply.length = length;
  };
  const auto accepts = [&](std::size_t length) { return command.size() == length; };

  switch (command[0])
  {
  case 'A':
    // Activate: the second byte is echoed; wired portals answer FF 77 after it.
    if (accepts(2))
    {
      echo(2, 10);
      m_queries.push(Report{'A', command[1], 0xFF, 0x77});
      portal.Activate();
    }
    break;

  case 'C':
    // Colour of the whole ring: R, G, B.
    if (accepts(4))
    {
      portal.SetLEDs(0x01, command[1], command[2], command[3]);
      echo(4, 12);
    }
    break;

  case 'J':
    // Fade one side to a colour: side, R, G, B and a duration.
    if (accepts(7))
    {
      echo(7, 15);
      m_queries.push(Report{'J'});
      portal.SetLEDs(command[1], command[2], command[3], command[4]);
    }
    break;

  case 'L':
    // Set one LED at once: side (0 right, 2 left, 1 trap), R, G, B. The side numbering differs
    // from SetLEDs', so the trap LED is not tracked.
    if (accepts(5))
    {
      echo(5, 13);
      const u8 side = command[1] == 0x02 ? 0x04 : command[1];
      portal.SetLEDs(side, command[2], command[3], command[4]);
    }
    break;

  case 'M':
    // Music/audio mode.
    if (accepts(2))
    {
      echo(2, 10);
      m_queries.push(Report{'M', command[1], 0x00, 0x19});
    }
    break;

  case 'Q':
    // Query (read) a block: figure index (low nibble), block.
    if (accepts(3))
    {
      Report report{};
      portal.QueryBlock(command[1] & 0xF, command[2], report.data());
      m_queries.push(report);
      echo(3, 11);
    }
    break;

  case 'R':
    // Ready (reset).
    if (accepts(2))
    {
      reply.control[0] = 'R';
      reply.length = 10;
      m_queries.push(Report{'R', 0x02, 0x1b});
    }
    break;

  case 'S':
    // Status: answered by the next status report.
    if (accepts(1))
      echo(1, 9);
    break;

  case 'V':
    if (accepts(4))
      echo(4, 12);
    break;

  case 'W':
    // Write a block: figure index (low nibble), block, 16 bytes.
    if (accepts(19))
    {
      Report report{};
      portal.WriteBlock(command[1] & 0xF, command[2], command.data() + 3, report.data());
      m_queries.push(report);
      echo(19, 27);
    }
    break;

  default:
    ERROR_LOG_FMT(IOS_USB, "Unhandled Skylander Portal Query: {}", command[0]);
    break;
  }
  return reply;
}

int SkylanderUSB::SubmitTransfer(std::unique_ptr<CtrlMessage> cmd)
{
  DEBUG_LOG_FMT(IOS_USB,
                "[{:04x}:{:04x} {}] Control: bRequestType={:02x} bRequest={} wValue={:04x}"
                " wIndex={:04x} wLength={:04x}",
                m_vid, m_pid, m_active_interface, cmd->request_type, cmd->request, cmd->value,
                cmd->index, cmd->length);

  if (cmd->request_type != HID_SET_REQUEST_TYPE)
    return IPC_EINVAL;

  CommandReply reply;
  if (cmd->request == HID_SET_IDLE || cmd->request == HID_SET_PROTOCOL)
  {
    reply.length = 8;
  }
  else if (cmd->request == HID_SET_REPORT)
  {
    auto& system = cmd->GetEmulationKernel().GetSystem();
    u8* const data = system.GetMemory().GetPointerForRange(cmd->data_address, cmd->length);
    if (cmd->length == 0 || data == nullptr)
    {
      ERROR_LOG_FMT(IOS_USB, "Skylander command invalid");
      return IPC_EINVAL;
    }
    reply = HandlePortalCommand(system.GetSkylanderPortal(), {data, cmd->length});
  }
  else
  {
    ERROR_LOG_FMT(IOS_USB, "Unhandled Request {}", cmd->request);
  }

  if (reply.length == 0)
    return IPC_EINVAL;

  ScheduleTransfer(std::move(cmd), reply.control, reply.length, CONTROL_DELAY_US);
  return 0;
}

int SkylanderUSB::SubmitTransfer(std::unique_ptr<BulkMessage> cmd)
{
  DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Bulk: length={} endpoint={}", m_vid, m_pid,
                m_active_interface, cmd->length, cmd->endpoint);
  return 0;
}

int SkylanderUSB::SubmitTransfer(std::unique_ptr<IntrMessage> cmd)
{
  DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Interrupt: length={} endpoint={}", m_vid, m_pid,
                m_active_interface, cmd->length, cmd->endpoint);

  auto& system = cmd->GetEmulationKernel().GetSystem();
  const u8* const data = system.GetMemory().GetPointerForRange(cmd->data_address, cmd->length);
  if (cmd->length == 0 || data == nullptr)
  {
    ERROR_LOG_FMT(IOS_USB, "Skylander command invalid");
    return IPC_EINVAL;
  }

  // Speaker audio (16-bit samples) goes out through this endpoint and is echoed back at once.
  if (cmd->length > REPORT_LENGTH && cmd->length <= MAX_AUDIO_LENGTH)
  {
    system.GetSoundStream()->GetMixer()->PushSkylanderPortalSamples(data, cmd->length / 2);
    Report echo{};
    std::copy_n(data, cmd->length, echo.begin());
    const auto length = static_cast<s32>(cmd->length);
    ScheduleTransfer(std::move(cmd), echo, length, 0);
    return 0;
  }

  // Otherwise the oldest queued reply, or else a status report.
  Report report;
  u64 delay_us;
  if (m_queries.empty())
  {
    report = system.GetSkylanderPortal().GetStatus();
    delay_us = STATUS_DELAY_US;
  }
  else
  {
    report = m_queries.front();
    m_queries.pop();
    delay_us = QUEUED_REPLY_DELAY_US;
  }
  ScheduleTransfer(std::move(cmd), report, REPORT_LENGTH, delay_us);
  return 0;
}

int SkylanderUSB::SubmitTransfer(std::unique_ptr<IsoMessage> cmd)
{
  DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Isochronous: length={} endpoint={} num_packets={}",
                m_vid, m_pid, m_active_interface, cmd->length, cmd->endpoint, cmd->num_packets);
  return 0;
}

void SkylanderUSB::ScheduleTransfer(std::unique_ptr<TransferCommand> command,
                                    const std::array<u8, 64>& data, s32 expected_count,
                                    u64 expected_time_us)
{
  command->FillBuffer(data.data(), static_cast<std::size_t>(expected_count));
  command->ScheduleTransferCompletion(expected_count, static_cast<u32>(expected_time_us));
}

// ---- the portal ----

void SkylanderPortal::Activate()
{
  std::lock_guard lock(m_mutex);
  if (m_activated)
    return;

  // Figures already standing on the portal are announced again.
  for (Skylander& slot : m_slots)
  {
    if ((slot.status & 1) != 0)
    {
      slot.queued_status.push(Skylander::ADDED);
      slot.queued_status.push(Skylander::READY);
    }
  }
  m_activated = true;
}

void SkylanderPortal::Deactivate()
{
  std::lock_guard lock(m_mutex);
  for (Skylander& slot : m_slots)
  {
    // Skip to the state the pending updates would end in.
    if (!slot.queued_status.empty())
    {
      slot.status = slot.queued_status.back();
      slot.queued_status = {};
    }
    slot.status &= 1;
  }
  m_activated = false;
}

bool SkylanderPortal::IsActivated()
{
  std::lock_guard lock(m_mutex);
  return m_activated;
}

// Once per session, every figure on the portal is reported as lifted and put back, so that the
// game sees it.
void SkylanderPortal::UpdateStatus()
{
  std::lock_guard lock(m_mutex);
  if (m_status_updated)
    return;

  for (Skylander& slot : m_slots)
  {
    if ((slot.status & 1) != 0)
    {
      slot.queued_status.push(Skylander::REMOVED);
      slot.queued_status.push(Skylander::ADDED);
      slot.queued_status.push(Skylander::READY);
    }
  }
  m_status_updated = true;
}

// Side: 0 right, 1 both, 2 left, 3 trap. Other values change nothing.
void SkylanderPortal::SetLEDs(u8 side, u8 red, u8 green, u8 blue)
{
  std::lock_guard lock(m_mutex);
  const SkylanderLEDColor color{red, green, blue};
  if (side == 0x00 || side == 0x01)
    m_color_right = color;
  if (side == 0x01 || side == 0x02)
    m_color_left = color;
  if (side == 0x03)
    m_color_trap = color;
}

// 'S', the 32-bit status word (two bits per slot, slot 0 lowest, little endian), a counter
// and whether the portal is active. Each call reports the next queued state of every slot.
std::array<u8, 64> SkylanderPortal::GetStatus()
{
  std::lock_guard lock(m_mutex);

  u32 word = 0;
  for (std::size_t i = m_slots.size(); i-- > 0;)
  {
    Skylander& slot = m_slots[i];
    if (!slot.queued_status.empty())
    {
      slot.status = slot.queued_status.front();
      slot.queued_status.pop();
    }
    word = (word << 2) | slot.status;
  }

  std::array<u8, 64> report{};
  report[0] = 'S';
  for (std::size_t b = 0; b < 4; ++b)
    report[1 + b] = static_cast<u8>(word >> (8 * b));
  report[5] = m_interrupt_counter++;
  report[6] = m_activated ? 0x01 : 0x00;
  return report;
}

// Reply: 'Q', 0x10 + slot (or 0x01 when no figure is ready there), block, 16 bytes of data.
void SkylanderPortal::QueryBlock(u8 sky_num, u8 block, u8* reply_buf)
{
  if (sky_num >= MAX_SKYLANDERS || block >= BLOCK_COUNT)
    return;

  std::lock_guard lock(m_mutex);
  const Skylander& slot = m_slots[sky_num];
  reply_buf[0] = 'Q';
  reply_buf[2] = block;
  if ((slot.status & Skylander::READY) == 0)
  {
    reply_buf[1] = 0x01;
    return;
  }
  reply_buf[1] = static_cast<u8>(0x10 | sky_num);
  slot.figure->GetBlock(block, reply_buf + 3);
}

// Reply: 'W', 0x10 + slot (or 0x01 when there is no figure), block. The figure file is saved
// after every write.
void SkylanderPortal::WriteBlock(u8 sky_num, u8 block, const u8* to_write_buf, u8* reply_buf)
{
  if (sky_num >= MAX_SKYLANDERS || block >= BLOCK_COUNT)
    return;

  std::lock_guard lock(m_mutex);
  Skylander& slot = m_slots[sky_num];
  reply_buf[0] = 'W';
  reply_buf[2] = block;
  if ((slot.status & 1) == 0)
  {
    reply_buf[1] = 0x01;
    return;
  }
  reply_buf[1] = static_cast<u8>(0x10 | sky_num);
  slot.figure->SetBlock(block, to_write_buf);
  slot.figure->Save();
}

bool SkylanderPortal::RemoveSkylander(u8 sky_num)
{
  if (sky_num >= MAX_SKYLANDERS)
    return false;

  DEBUG_LOG_FMT(IOS_USB, "Cleared Skylander from slot {}", sky_num);
  std::lock_guard lock(m_mutex);
  Skylander& slot = m_slots[sky_num];

  if (slot.figure != nullptr && slot.figure->FileIsOpen())
  {
    slot.figure->Save();
    slot.figure->Close();
  }

  if ((slot.status & Skylander::READY) == 0)
    return false;

  slot.status = Skylander::REMOVING;
  slot.queued_status.push(Skylander::REMOVING);
  slot.queued_status.push(Skylander::REMOVED);
  return true;
}

// A figure goes back to the free slot it last stood on, or else to the first free slot.
u8 SkylanderPortal::LoadSkylander(std::unique_ptr<SkylanderFigure> figure)
{
  std::lock_guard lock(m_mutex);

  std::array<u8, BLOCK_SIZE> first_block{};
  figure->GetBlock(0, first_block.data());
  const u32 serial = static_cast<u32>(first_block[0]) | (static_cast<u32>(first_block[1]) << 8) |
                     (static_cast<u32>(first_block[2]) << 16) |
                     (static_cast<u32>(first_block[3]) << 24);

  std::size_t chosen = m_slots.size();
  for (std::size_t i = 0; i < m_slots.size(); ++i)
  {
    if ((m_slots[i].status & 1) != 0)
      continue;
    if (chosen == m_slots.size())
      chosen = i;
    if (m_slots[i].last_id == serial)
    {
      chosen = i;
      break;
    }
  }

  if (chosen == m_slots.size())
    return 0xFF;

  DEBUG_LOG_FMT(IOS_USB, "Placing figure {:08x} in slot {}", serial, chosen);
  Skylander& slot = m_slots[chosen];
  slot.figure = std::move(figure);
  slot.status = Skylander::ADDED;
  slot.queued_status.push(Skylander::ADDED);
  slot.queued_status.push(Skylander::READY);
  slot.last_id = serial;
  return static_cast<u8>(chosen);
}

// Figure ID (block 1, bytes 0-1) and variant (block 1, bytes 12-13), little endian.
std::pair<u16, u16> SkylanderPortal::CalculateIDs(const std::array<u8, 0x40 * 0x10>& file_data)
{
  const auto read16 = [&](std::size_t offset) {
    return static_cast<u16>(file_data[offset] | (file_data[offset + 1] << 8));
  };
  return {read16(0x10), read16(0x1C)};
}

Skylander* SkylanderPortal::GetSkylander(u8 slot)
{
  return m_slots.data() + slot;
}

Type NormalizeSkylanderType(Type type)
{
  switch (type)
  {
  case Type::Skylander:
  case Type::Giant:
  case Type::Swapper:
  case Type::TrapMaster:
  case Type::Mini:
    return Type::Skylander;
  case Type::Trophy:
    return Type::Trophy;
  default:
    // Items keep no data; traps and vehicles are not supported yet.
    return Type::Unknown;
  }
}
}  // namespace IOS::HLE::USB
