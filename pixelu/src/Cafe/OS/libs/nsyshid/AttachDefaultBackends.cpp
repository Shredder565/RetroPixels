// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "nsyshid.h"
#include "Backend.h"
#include "BackendEmulated.h"
#ifdef HAS_LIBUSB
#include "BackendLibusb.h"
#endif

namespace nsyshid::backend
{
	template<typename T>
	static void TryAttach()
	{
		auto backend = std::make_shared<T>();
		if (backend->IsInitialisedOk())
			AttachBackend(backend);
	}

	void AttachDefaultBackends()
	{
		// real devices first, so the emulated backend can see which portals are connected
#ifdef HAS_LIBUSB
		TryAttach<libusb::BackendLibusb>();
#endif
		TryAttach<emulated::BackendEmulated>();
	}
} // namespace nsyshid::backend
