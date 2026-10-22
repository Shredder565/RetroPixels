// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/HRWrap.h"

namespace Common
{
std::string GetHResultMessage(HRESULT hr)
{
  return winrt::to_string(winrt::hresult_error(hr).message());
}
}  // namespace Common
