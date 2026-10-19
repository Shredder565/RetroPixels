// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// vsync rate override (graphic packs); a frequency <= 0 means the default rate
void LatteTiming_setCustomVsyncFrequency(sint32 frequency);
void LatteTiming_disableCustomVsyncFrequency();
bool LatteTiming_getCustomVsyncFrequency(sint32& customFrequency);

// let the host display's vsync drive the emulated vsync (Vulkan only)
void LatteTiming_EnableHostDrivenVSync();
