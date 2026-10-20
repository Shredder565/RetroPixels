// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/BitSet.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"

#include "VideoCommon/BPMemory.h"

// A model of which texture units would still hold their texture in the console's texture
// memory (TMEM) from one draw to the next. The texture cache uses it to skip rehashing, and to
// keep serving a texture that a game overwrote in RAM without invalidating TMEM.
namespace TMEM
{
// Forget everything held by every unit.
void InvalidateAll();
// A game-issued TMEM invalidate (its argument is not decoded; everything is dropped).
void Invalidate(u32 param);
// A BP write touched texture-unit registers.
void ConfigurationChanged(TexUnitAddress bp_addr, u32 config);
// A texture of the given size in 4x4/8x4/8x8 blocks was bound to `unit` for the next draw.
void Bind(u32 unit, int num_blocks_width, int num_blocks_height, bool is_mipmapped, bool is_32_bit);
// After all units of a draw are bound: units whose cache regions collide cannot keep data.
void FinalizeBinds(BitSet32 used_textures);
// The unit holds all of its texture.
bool IsCached(u32 unit);
// The unit's configuration has not changed since its last bind.
bool IsValid(u32 unit);

void Init();
void DoState(PointerWrap& p);

}  // namespace TMEM
