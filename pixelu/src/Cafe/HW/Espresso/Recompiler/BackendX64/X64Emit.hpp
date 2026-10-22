// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "x86Emitter.h"

// small shared pieces of the x86-64 backend

namespace X64Backend
{
	inline x64::Cond ToHostCond(IMLCond c)
	{
		switch (c)
		{
		case IMLCond::EQ: return x64::CC_E;
		case IMLCond::NE: return x64::CC_NE;
		case IMLCond::SLT: return x64::CC_L;
		case IMLCond::SGT: return x64::CC_G;
		case IMLCond::SLE: return x64::CC_LE;
		case IMLCond::SGE: return x64::CC_GE;
		case IMLCond::ULT: return x64::CC_B;
		case IMLCond::UGT: return x64::CC_A;
		case IMLCond::ULE: return x64::CC_BE;
		case IMLCond::UGE: return x64::CC_AE;
		}
		return x64::CC_E;
	}

	// condition codes come in pairs that differ in the lowest bit
	inline x64::Cond InvertHostCond(x64::Cond c)
	{
		return (x64::Cond)(c ^ 1);
	}
}
