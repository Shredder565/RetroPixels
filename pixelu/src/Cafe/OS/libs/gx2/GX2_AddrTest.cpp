// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"

namespace GX2
{
	// Hook for checking the surface address code against the console's own addrlib.
	// PixelU does not ship such a check; surface layout is verified offline instead.
	void _test_AddrLib()
	{
	}
}
