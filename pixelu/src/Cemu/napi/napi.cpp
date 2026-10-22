// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "napi.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "config/ActiveSettings.h"

NetworkService NAPI::AuthInfo::GetService() const
{
	if (serviceOverwrite.has_value())
		return *serviceOverwrite;
	return ActiveSettings::GetNetworkService();
}
