// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The ROM images of a loaded game, one buffer per named region ("crom",
// "banked_crom", "vrom", "sound_program", ...), each with the patches
// Games.xml asks for.

#ifndef INCLUDED_ROMSET_H
#define INCLUDED_ROMSET_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct ROM
{
  // A value written over the image, most significant byte first.
  struct BigEndianPatch
  {
    uint32_t offset;
    uint64_t value;
    unsigned bits;    // 8, 16, 32 or 64

    BigEndianPatch(uint32_t o, uint64_t v, unsigned b)
      : offset(o), value(v), bits(b)
    {
    }
  };

  std::shared_ptr<uint8_t> data;
  std::vector<BigEndianPatch> patches;
  size_t size = 0;

  // Copies up to dest_size bytes of the image to dest and, unless told not
  // to, applies the patches that fit inside dest_size.
  void CopyTo(uint8_t *dest, size_t dest_size, bool apply_patches = true) const;
};

struct ROMSet
{
  std::map<std::string, ROM> rom_by_region;

  // The region's ROM, or an empty ROM (size 0) if the game has none.
  ROM get_rom(const std::string &region) const;
};

#endif // INCLUDED_ROMSET_H
