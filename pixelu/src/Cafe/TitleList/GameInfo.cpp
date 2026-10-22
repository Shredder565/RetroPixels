// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "GameInfo.h"
#include "config/ActiveSettings.h"

fs::path GameInfo2::GetSaveFolder()
{
	const TitleId titleId = GetBaseTitleId();
	return ActiveSettings::GetMlcPath(fmt::format("usr/save/{:08x}/{:08x}", titleId >> 32, titleId & 0xFFFFFFFF));
}
