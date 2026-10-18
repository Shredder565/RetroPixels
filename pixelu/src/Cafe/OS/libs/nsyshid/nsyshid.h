// Original code for RetroPixels (PixelU). Not derived from Cemu.
// nsyshid: the console's USB HID library (toy portals and other HID devices).
#pragma once
#include "Cafe/OS/RPL/COSModule.h"

namespace nsyshid
{
	class Backend;

	void AttachBackend(const std::shared_ptr<Backend>& backend);
	void DetachBackend(const std::shared_ptr<Backend>& backend);

	COSModule* GetModule();
} // namespace nsyshid
