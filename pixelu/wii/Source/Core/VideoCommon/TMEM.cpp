// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/TMEM.h"

#include <array>

#include "Common/ChunkFile.h"
#include "VideoCommon/BPMemory.h"

// How the model works
// -------------------
// Each of the eight texture units points two cache regions ("banks") into TMEM: the even bank
// always holds level 0 (and the other even mip levels), the odd bank is used for odd mip levels
// and for the second half of 32-bit texels. A game sets a region's offset and its size code
// through the SETIMAGE1/SETIMAGE2 registers, and regions may overlap freely.
//
// A unit is "cached" after a bind when the bound texture fits entirely in its region(s) and no
// other live region overlaps them; then a following draw would sample stale TMEM rather than
// RAM, which some games rely on after overwriting a texture with an EFB copy. A unit is "valid"
// while its registers are unchanged, and "invalid" once they change or TMEM is flushed.
//
// This is an estimate. Real hardware streams texels through these regions as it samples, so
// whether a texture is fully resident also depends on what was drawn.

namespace TMEM
{
namespace
{
enum class Residency
{
  Invalid,  // registers changed (or TMEM flushed) since the last bind
  Valid,    // bound, but too large for its regions or sharing them with another unit
  Cached,   // bound, and the whole texture stays resident
};

// A unit's cache region. `size` is in bytes and is only known after a bind; zero means unused.
struct Region
{
  u32 width_code = 0;
  u32 height_code = 0;
  u32 offset = 0;
  u32 size = 0;
};

// Saved verbatim in savestates: two regions, then the residency. Do not reorder.
struct UnitState
{
  Region even;
  Region odd;
  Residency residency = Residency::Invalid;
};

std::array<UnitState, 8> s_units;

// Region size from its two 3-bit codes. The SDK only uses the three square presets; anything
// else follows the guess that each code doubles one dimension of a 512-byte base.
u32 RegionBytes(const Region& region)
{
  if (region.width_code == region.height_code)
  {
    constexpr u32 kib = 1024;
    if (region.width_code == 3)
      return 32 * kib;
    if (region.width_code == 4)
      return 128 * kib;
    if (region.width_code == 5)
      return 512 * kib;
  }
  return 512u << (region.width_code + region.height_code);
}

// Half-open byte ranges [offset, offset + size) intersect. Unused regions never do.
bool RangesCollide(const Region& a, const Region& b)
{
  if (a.size == 0 || b.size == 0)
    return false;
  const auto starts_within = [](const Region& outer, const Region& inner) {
    return outer.offset <= inner.offset && outer.offset + outer.size > inner.offset;
  };
  return starts_within(a, b) || starts_within(b, a);
}

bool UnitsCollide(const UnitState& a, const UnitState& b)
{
  if (a.residency == Residency::Invalid || b.residency == Residency::Invalid)
    return false;
  for (const Region* ra : {&a.even, &a.odd})
  {
    for (const Region* rb : {&b.even, &b.odd})
    {
      if (RangesCollide(*ra, *rb))
        return true;
    }
  }
  return false;
}
}  // namespace

void ConfigurationChanged(TexUnitAddress bp_addr, u32 config)
{
  UnitState& unit = s_units[bp_addr.GetUnitID()];
  unit.residency = Residency::Invalid;

  // Only the image registers move a region; TMEM offsets are given in 32-byte lines.
  if (bp_addr.Reg == TexUnitAddress::Register::SETIMAGE1)
  {
    const TexImage1 reg{.hex = config};
    unit.even = Region{reg.cache_width, reg.cache_height, reg.tmem_even * 32u, 0};
  }
  else if (bp_addr.Reg == TexUnitAddress::Register::SETIMAGE2)
  {
    const TexImage2 reg{.hex = config};
    unit.odd = Region{reg.cache_width, reg.cache_height, reg.tmem_odd * 32u, 0};
  }
}

void InvalidateAll()
{
  for (UnitState& unit : s_units)
    unit.residency = Residency::Invalid;
}

void Invalidate(u32 /* param */)
{
  // The parameter seems to encode an address and a length, but its format is not known well
  // enough to invalidate selectively.
  InvalidateAll();
}

void Bind(u32 unit_index, int width, int height, bool is_mipmapped, bool is_32_bit)
{
  UnitState& unit = s_units[unit_index];

  // Every block (4x4, 8x4 or 8x8 texels) occupies one 32-byte TMEM line per bank.
  const u32 needed = static_cast<u32>(width) * static_cast<u32>(height) * 32u;
  const bool uses_odd = is_mipmapped || is_32_bit;

  unit.even.size = RegionBytes(unit.even);
  unit.odd.size = uses_odd ? RegionBytes(unit.odd) : 0;

  bool fits = needed <= unit.even.size;
  if (uses_odd)
    fits = fits && needed <= unit.odd.size;

  // Mipmapped textures are treated as also occupying a second region's worth of space after
  // the even region (and after the odd region for 32-bit textures). This matches what games
  // appear to expect, though the true footprint has not been measured on hardware.
  if (is_mipmapped)
  {
    unit.even.size *= 2;
    if (is_32_bit)
      unit.odd.size *= 2;
  }

  unit.residency = fits ? Residency::Cached : Residency::Valid;
}

void FinalizeBinds(BitSet32 used_textures)
{
  // A collision only needs both units to be live, and nothing here makes a unit invalid, so
  // the result does not depend on the order the pairs are visited in.
  for (const int bit : used_textures)
  {
    const u32 i = static_cast<u32>(bit);
    UnitState& unit = s_units[i];
    if (RangesCollide(unit.even, unit.odd))
      unit.residency = Residency::Valid;

    for (u32 j = 0; j < s_units.size(); ++j)
    {
      if (j == i || !UnitsCollide(unit, s_units[j]))
        continue;
      unit.residency = Residency::Valid;
      s_units[j].residency = Residency::Valid;
    }
  }
}

bool IsCached(u32 unit)
{
  return s_units[unit].residency == Residency::Cached;
}

bool IsValid(u32 unit)
{
  return s_units[unit].residency != Residency::Invalid;
}

void Init()
{
  s_units.fill(UnitState{});
}

void DoState(PointerWrap& p)
{
  p.DoArray(s_units);
}

}  // namespace TMEM
