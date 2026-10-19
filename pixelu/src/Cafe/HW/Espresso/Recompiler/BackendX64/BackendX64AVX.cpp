// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// The x86-64 backend only emits SSE2 code; this file is kept as the place
// for code paths that need AVX. Nothing currently requires it, the results
// of the SSE2 code are what the interpreter computes.

#include "BackendX64.h"
