// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "ROMSet.h"
#include "OSD/Logger.h"

#include <cstring>

void ROM::CopyTo(uint8_t *dest, size_t dest_size, bool apply_patches) const
{
  if (dest == nullptr || dest_size == 0 || !data || size == 0)
    return;

  std::memcpy(dest, data.get(), size < dest_size ? size : dest_size);
  if (!apply_patches)
    return;

  for (const BigEndianPatch &p : patches)
  {
    size_t width = p.bits / 8;
    if (size_t(p.offset) + width > dest_size)
    {
      ErrorLog("Ignored ROM patch to offset 0x%x in region 0x%x bytes long.", p.offset, dest_size);
      continue;
    }
    if (p.bits != 8 && p.bits != 16 && p.bits != 32 && p.bits != 64)
      continue;
    // Most significant byte at the lowest address.
    for (size_t b = 0; b < width; b++)
      dest[p.offset + b] = uint8_t(p.value >> (8 * (width - 1 - b)));
  }
}

ROM ROMSet::get_rom(const std::string &region) const
{
  auto found = rom_by_region.find(region);
  if (found != rom_by_region.end())
    return found->second;
  return ROM();
}
