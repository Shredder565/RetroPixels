// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

namespace coreinit
{
	// the title's code generation (JIT) area; both 0 until the title asked for it
	void OSGetCodegenVirtAddrRangeInternal(uint32& rangeStart, uint32& rangeSize);
	// instruction cache block invalidate for the 32 byte line at ea
	void codeGenHandleICBI(uint32 ea);
	// true once the title switched the area to writable mode; the recompiler then leaves it alone
	bool codeGenShouldAvoid();

	void InitializeCodeGen();
}
