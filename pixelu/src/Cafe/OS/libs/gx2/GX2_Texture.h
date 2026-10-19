// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "GX2_Surface.h"

namespace GX2
{
	// Guest layout of GX2Texture: a surface plus a view of it and the precomputed
	// SQ_TEX_RESOURCE words. Words 2 and 3 (image/mip address) are filled in when the
	// texture is bound, so they are not part of the structure.
	struct GX2Texture
	{
		/* +0x00 */ GX2Surface surface;
		/* +0x74 */ uint32be viewFirstMip;
		/* +0x78 */ uint32be viewNumMips;
		/* +0x7C */ uint32be viewFirstSlice;
		/* +0x80 */ uint32be viewNumSlices;
		/* +0x84 */ uint32be compSel; // one byte per channel, R in the top byte
		/* +0x88 */ betype<Latte::LATTE_SQ_TEX_RESOURCE_WORD0_N> regTexWord0;
		/* +0x8C */ betype<Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N> regTexWord1;
		/* +0x90 */ betype<Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N> regTexWord4;
		/* +0x94 */ betype<Latte::LATTE_SQ_TEX_RESOURCE_WORD5_N> regTexWord5;
		/* +0x98 */ betype<Latte::LATTE_SQ_TEX_RESOURCE_WORD6_N> regTexWord6;
	};
	static_assert(offsetof(GX2Texture, viewFirstMip) == 0x74);
	static_assert(offsetof(GX2Texture, regTexWord0) == 0x88);
	static_assert(sizeof(GX2Texture) == 0x9C);

	// Guest layout of GX2Sampler: the three SQ_TEX_SAMPLER words
	struct GX2Sampler
	{
		/* +0x0 */ betype<Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0> word0;
		/* +0x4 */ betype<Latte::LATTE_SQ_TEX_SAMPLER_WORD1_0> word1;
		/* +0x8 */ betype<Latte::LATTE_SQ_TEX_SAMPLER_WORD2_0> word2;
	};
	static_assert(sizeof(GX2Sampler) == 0xC);

	void GX2InitTextureRegs(GX2Texture* texture);

	void GX2TextureInit();
};
