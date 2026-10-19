// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

namespace coreinit
{
	// callee saved register state of a coroutine (OSInitCoroutine/OSSwitchCoroutine)
	struct OSCoroutine
	{
		/* +0x00 */ uint32be lr;
		/* +0x04 */ uint32be cr;
		/* +0x08 */ uint32be gqr1;
		/* +0x0C */ uint32be r1; // stack pointer
		/* +0x10 */ uint32be r2;
		/* +0x14 */ uint32be r13;
		/* +0x18 */ uint32be gpr[18]; // r14-r31
		/* +0x60 */ uint64be fpr[18]; // f14-f31, paired single slot 0
		/* +0xF0 */ uint64be psr[18]; // f14-f31, paired single slot 1
	};
	static_assert(offsetof(OSCoroutine, gpr) == 0x18);
	static_assert(offsetof(OSCoroutine, fpr) == 0x60);
	static_assert(offsetof(OSCoroutine, psr) == 0xF0);
	static_assert(sizeof(OSCoroutine) == 0x180);

	void InitializeCoroutine();
}
