// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// W43 harness shim: the old AMMediaboard.cpp includes Common/UnixUtil.h, which the tree no
// longer has. Retries a call while it fails with EINTR.

#pragma once
#include <cerrno>
namespace UnixUtil
{
template <typename Fn, typename... Args>
auto RetryOnEINTR(Fn fn, Args... args)
{
  for (;;)
  {
    const auto r = fn(args...);
    if (!(r == -1 && errno == EINTR))
      return r;
  }
}
}  // namespace UnixUtil
