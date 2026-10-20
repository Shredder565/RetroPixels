// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// Asks the compiler to inline a function even where its heuristics would not.
#if defined(_MSC_VER)
#define DOLPHIN_FORCE_INLINE __forceinline
#else
#define DOLPHIN_FORCE_INLINE inline __attribute__((always_inline))
#endif
