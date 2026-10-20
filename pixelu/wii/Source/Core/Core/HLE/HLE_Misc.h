// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

namespace Core
{
class CPUThreadGuard;
}

namespace HLE_Misc
{
void UnimplementedFunction(const Core::CPUThreadGuard& guard);
void HBReload(const Core::CPUThreadGuard& guard);
void GeckoCodeHandlerICacheFlush(const Core::CPUThreadGuard& guard);
void GeckoReturnTrampoline(const Core::CPUThreadGuard& guard);
}  // namespace HLE_Misc
