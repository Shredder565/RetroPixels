// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// Host fault interception for the JIT. While installed, access violations and
// stack overflows on the CPU thread are first offered to the JIT (fastmem
// backpatching, stack guard pages) before the platform's normal handling.
namespace EMM
{
void InstallExceptionHandler();
void UninstallExceptionHandler();
bool IsExceptionHandlerSupported();
}  // namespace EMM
