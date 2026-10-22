// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/MMU/MMU.h"

// Video interface registers. Scan-out is emulated by GX2/Latte, so no VI
// register is backed here; unhandled MMIO accesses read as zero.
