// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

class PointerWrap;
struct Sram;
namespace Core
{
class System;
}

// Brings the emulated console's hardware up and down as one unit, and
// saves or loads all of its device state.
namespace HW
{
// `override_sram`, when not null, replaces the SRAM loaded from disk.
void Init(Core::System& system, const Sram* override_sram);
void Shutdown(Core::System& system);
void DoState(Core::System& system, PointerWrap& p);
}  // namespace HW
