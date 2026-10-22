// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Developer-only helpers. In release builds (no CEMU_DEBUG_ASSERT) debug_printf
// expands to nothing; debugBreakpoint() is a no-op hook in every build that a
// developer can set a native breakpoint on.

#ifdef CEMU_DEBUG_ASSERT
#define debug_printf(...) printf(__VA_ARGS__)
#else
#define debug_printf(...)
#endif

inline void debugBreakpoint()
{
}
