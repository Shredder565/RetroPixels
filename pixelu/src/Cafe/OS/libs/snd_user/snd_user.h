// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/RPL/COSModule.h"

// snd_user / snduser2: the MIX helper layer over AX, sound tables and AXFX.
namespace snd_user
{
	// speaker layout chosen with MIXSetSoundMode / MIXSetDeviceSoundMode
	enum class MixSoundMode : uint32
	{
		Mono = 0,
		Stereo = 1,
		Surround = 2,
		SurroundDPL2 = 3,
		Surround6CH = 4
	};

	COSModule* GetModuleSndUser1();
	COSModule* GetModuleSndUser2();
}
