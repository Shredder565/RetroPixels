// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// x86-64 backend: instruction choices that depend on optional CPU features
// (LZCNT, MOVBE).

#include "Common/cpu_features.h"
#include "BackendX64.h"

using namespace x64;

namespace X64Backend
{
	void CodeGenerator::EmitCountLeadingZeros(uint8 dst, uint8 src)
	{
		if (g_CPUFeatures.x86.lzcnt)
		{
			m_e.LZCNT(dst, src);
			return;
		}
		// BSR gives the index of the highest set bit (undefined for 0)
		m_e.MOV_ri32(RCX, 63);
		m_e.BSR(RAX, src);
		m_e.CMOV_rr(CC_E, RAX, RCX);
		m_e.ALU_ri(ALU_XOR, RAX, 31);
		m_e.MOV_rr(dst, RAX);
	}

	void CodeGenerator::EmitLoadSwapped32(uint8 dst, const x64::Mem& m)
	{
		if (g_CPUFeatures.x86.movbe)
		{
			m_e.EncodeRM(0, false, {0x0F, 0x38, 0xF0}, dst, m);
			return;
		}
		m_e.MOV_rm(dst, m);
		m_e.BSWAP(dst);
	}
}
